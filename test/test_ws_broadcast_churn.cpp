// P0 regression: concurrent WsPushHub broadcasts while WebSocket clients
// rapidly connect / disconnect (abortive RST closes recycle server-side fds).
//
// Verifies:
//   * no use-after-free (run under ASan/TSan): the hub never dereferences a
//     destroyed app_ws_data_process,
//   * zero foreign payload bytes: every frame a client receives is addressed
//     to its own user or to everybody,
//   * exact accounting for surviving clients: every stable client receives
//     exactly the frames broadcast to it, in per-producer order,
//   * the hub drains back to the stable set once churn stops.
#include "server.h"
#include "multi_protocol_factory.h"
#include "app_handler_v2.h"
#include "ws_push_hub.h"
#include "outbound_guard.h"
#include "ws_test_client.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class QuietApp : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest&, myframe::HttpResponse& res) override {
        res.status = 404;
        res.body = "nf";
    }
    void on_ws(const myframe::WsFrame&, myframe::WsFrame& send) override {
        send = myframe::WsFrame::text("");
    }
};

int env_int(const char* name, int def) {
    const char* e = std::getenv(name);
    int v = e ? std::atoi(e) : 0;
    return v > 0 ? v : def;
}

// Payload format: "<target>|<producer>|<seq>|<padding>"
std::string make_payload(const std::string& target, int producer, int seq) {
    std::string s = target + "|" + std::to_string(producer) + "|" + std::to_string(seq) + "|";
    s.append(64, 'x');
    return s;
}

bool parse_payload(const std::string& p, std::string& target, int& producer, int& seq) {
    size_t a = p.find('|');
    if (a == std::string::npos) return false;
    size_t b = p.find('|', a + 1);
    if (b == std::string::npos) return false;
    size_t c = p.find('|', b + 1);
    if (c == std::string::npos) return false;
    target = p.substr(0, a);
    producer = std::atoi(p.substr(a + 1, b - a - 1).c_str());
    seq = std::atoi(p.substr(b + 1, c - b - 1).c_str());
    return true;
}

struct StableResult {
    std::atomic<long> own{0};
    std::atomic<long> all{0};
    std::atomic<long> foreign{0};
    std::atomic<long> order_violations{0};
    std::atomic<bool> closed{false};
};

} // namespace

int main() {
    const int kStable = env_int("CHURN_STABLE", 8);
    const int kChurnThreads = env_int("CHURN_THREADS", 4);
    const int kProducers = 2;
    const int kRounds = env_int("CHURN_ROUNDS", 1500);
    const int kPaceEvery = env_int("CHURN_PACE_EVERY", 2);

    QuietApp app;
    auto factory = std::make_shared<MultiProtocolFactory>(&app, MultiProtocolFactory::Mode::PlainOnly);
    const unsigned short port = wstest::pick_free_port();
    server srv(4);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    myframe::reset_outbound_stats();

    // ---- stable clients -------------------------------------------------
    std::vector<std::unique_ptr<wstest::Client> > stable(kStable);
    std::vector<std::unique_ptr<StableResult> > results;
    for (int i = 0; i < kStable; ++i) {
        stable[i].reset(new wstest::Client());
        results.emplace_back(new StableResult());
        CHECK(stable[i]->connect_ws(port, "stable" + std::to_string(i)));
    }
    // Wait until every stable connection is registered with the hub.
    for (int t = 0; t < 400; ++t) {
        bool all = true;
        for (int i = 0; i < kStable; ++i) {
            if (WsPushHub::Instance().UserConnectionCount("stable" + std::to_string(i)) != 1) all = false;
        }
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < kStable; ++i) {
        CHECK(WsPushHub::Instance().UserConnectionCount("stable" + std::to_string(i)) == 1);
    }

    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < kStable; ++i) {
        readers.emplace_back([&, i] {
            const std::string me = "stable" + std::to_string(i);
            std::vector<int> last_own(kProducers, -1), last_all(kProducers, -1);
            StableResult& r = *results[i];
            stable[i]->set_timeout(100);
            while (!stop_readers.load()) {
                std::string payload; int op = 0;
                int rc = stable[i]->read_frame(payload, op);
                if (rc == 0) continue;
                if (rc < 0) { r.closed = true; break; }
                if (op != 0x1) continue;
                std::string target; int producer = -1, seq = -1;
                if (!parse_payload(payload, target, producer, seq) || producer < 0 || producer >= kProducers) {
                    ++r.foreign;
                    continue;
                }
                if (target == me) {
                    if (seq <= last_own[producer]) ++r.order_violations;
                    last_own[producer] = seq;
                    ++r.own;
                } else if (target == "ALL") {
                    if (seq <= last_all[producer]) ++r.order_violations;
                    last_all[producer] = seq;
                    ++r.all;
                } else {
                    ++r.foreign;
                }
            }
        });
    }

    // ---- churn clients --------------------------------------------------
    std::atomic<bool> stop_churn{false};
    std::atomic<long> churn_connects{0}, churn_foreign{0}, churn_frames{0};
    std::vector<std::thread> churners;
    for (int c = 0; c < kChurnThreads; ++c) {
        churners.emplace_back([&, c] {
            const std::string me = "churn" + std::to_string(c);
            int iter = 0;
            while (!stop_churn.load()) {
                wstest::Client cl;
                if (!cl.connect_ws(port, me, 0, 2000)) continue;
                ++churn_connects;
                cl.set_timeout(5);
                // Read a few frames, then drop the connection (alternate RST / FIN).
                int want = (iter % 5);
                for (int k = 0; k < want; ++k) {
                    std::string payload; int op = 0;
                    int rc = cl.read_frame(payload, op);
                    if (rc <= 0) break;
                    if (op != 0x1) continue;
                    ++churn_frames;
                    std::string target; int producer, seq;
                    if (!parse_payload(payload, target, producer, seq) || (target != me && target != "ALL")) {
                        ++churn_foreign;
                    }
                }
                if (iter++ % 2) cl.abort_close(); else cl.close_fd();
            }
        });
    }

    // ---- producers ------------------------------------------------------
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int seq = 0; seq < kRounds; ++seq) {
                for (int i = 0; i < kStable; ++i) {
                    const std::string u = "stable" + std::to_string(i);
                    WsPushHub::Instance().BroadcastToUser(u, make_payload(u, p, seq));
                }
                for (int c = 0; c < kChurnThreads; ++c) {
                    const std::string u = "churn" + std::to_string(c);
                    WsPushHub::Instance().BroadcastToUser(u, make_payload(u, p, seq));
                }
                WsPushHub::Instance().BroadcastAll(make_payload("ALL", p, seq));
                // Pace producers so connect/disconnect churn overlaps the
                // whole broadcast phase.
                if ((seq % kPaceEvery) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    for (auto& t : producers) t.join();
    stop_churn = true;
    for (auto& t : churners) t.join();

    // ---- exact accounting for survivors --------------------------------
    const long expected_each = static_cast<long>(kProducers) * kRounds;
    for (int t = 0; t < 3000; ++t) {
        bool done = true;
        for (int i = 0; i < kStable; ++i) {
            if (results[i]->own.load() < expected_each || results[i]->all.load() < expected_each) done = false;
        }
        if (done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give any (erroneous) extra frames a moment to show up.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_readers = true;
    for (auto& t : readers) t.join();

    long total_own = 0, total_all = 0;
    for (int i = 0; i < kStable; ++i) {
        StableResult& r = *results[i];
        total_own += r.own.load();
        total_all += r.all.load();
        if (r.own.load() != expected_each || r.all.load() != expected_each || r.foreign.load() != 0 ||
            r.order_violations.load() != 0 || r.closed.load()) {
            std::fprintf(stderr, "stable%d own=%ld all=%ld foreign=%ld order=%ld closed=%d expected=%ld\n",
                         i, r.own.load(), r.all.load(), r.foreign.load(), r.order_violations.load(),
                         r.closed.load() ? 1 : 0, expected_each);
        }
        CHECK(r.own.load() == expected_each);
        CHECK(r.all.load() == expected_each);
        CHECK(r.foreign.load() == 0);
        CHECK(r.order_violations.load() == 0);
        CHECK(!r.closed.load());
    }
    CHECK(churn_foreign.load() == 0);
    CHECK(churn_connects.load() > 0);

    // Churned connections unregister on close; only the stable set remains.
    for (int t = 0; t < 300 && WsPushHub::Instance().ConnectionCount() != static_cast<size_t>(kStable); ++t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(WsPushHub::Instance().ConnectionCount() == static_cast<size_t>(kStable));
    myframe::OutboundStats st = myframe::outbound_stats();
    CHECK(st.ws_slow_consumer_closed == 0);

    for (auto& c : stable) c->close_fd();
    srv.stop();
    srv.join();

    std::printf("ws_broadcast_churn: stable=%d producers=%d rounds=%d churn_connects=%ld churn_frames=%ld "
                "stable_frames=%ld (own=%ld all=%ld) foreign=0 -> %s\n",
                kStable, kProducers, kRounds, churn_connects.load(), churn_frames.load(),
                total_own + total_all, total_own, total_all, g_fail.load() ? "FAIL" : "PASS");
    return g_fail.load() ? 1 : 0;
}
