// P0 regression: outbound backpressure against a stalled WebSocket receiver.
//
// A "slow" client completes the handshake with a tiny receive buffer and then
// never reads. The test pushes far more data at it than the high-water mark
// allows and checks that
//   * the slow consumer is detected and closed (counter + hub unregister),
//   * queued memory stays bounded (process RSS growth << bytes pushed),
//   * a healthy client on the same server still receives every frame.
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
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define MYFRAME_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MYFRAME_TEST_ASAN 1
#endif
#endif

#ifdef MYFRAME_TEST_ASAN
// Keep ASan's free-memory quarantine small so RSS reflects live memory.
extern "C" const char* __asan_default_options() { return "quarantine_size_mb=8"; }
#endif

namespace {

std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class QuietApp : public myframe::IApplicationHandler {
public:
    void on_ws(const myframe::WsFrame&, myframe::WsFrame& send) override { send = myframe::WsFrame::text(""); }
};

size_t rss_bytes() {
    std::ifstream f("/proc/self/statm");
    size_t pages_total = 0, pages_rss = 0;
    f >> pages_total >> pages_rss;
    return pages_rss * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

} // namespace

int main() {
    // Default HWM is 4 MiB unless overridden by the environment.
    if (!std::getenv("MYFRAME_OUTBOUND_HWM_BYTES")) {
        CHECK(myframe::outbound_high_water_mark() == myframe::kDefaultOutboundHighWaterMark);
        CHECK(myframe::kDefaultOutboundHighWaterMark == 4u * 1024u * 1024u);
    }
    const size_t kHwm = 1u * 1024u * 1024u;
    myframe::set_outbound_high_water_mark(kHwm);
    CHECK(myframe::outbound_high_water_mark() == kHwm);
    myframe::reset_outbound_stats();

    QuietApp app;
    auto factory = std::make_shared<MultiProtocolFactory>(&app, MultiProtocolFactory::Mode::PlainOnly);
    const unsigned short port = wstest::pick_free_port();
    server srv(3);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    wstest::Client slow, fast;
    CHECK(slow.connect_ws(port, "slow", 4096));
    CHECK(fast.connect_ws(port, "fast"));
    for (int t = 0; t < 300; ++t) {
        if (WsPushHub::Instance().UserConnectionCount("slow") == 1 &&
            WsPushHub::Instance().UserConnectionCount("fast") == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(WsPushHub::Instance().UserConnectionCount("slow") == 1);
    CHECK(WsPushHub::Instance().UserConnectionCount("fast") == 1);

    // Healthy reader.
    const int kFastFrames = 2000;
    std::atomic<int> fast_frames{0};
    std::atomic<bool> fast_bad{false};
    std::thread fast_reader([&] {
        fast.set_timeout(200);
        int idle = 0;
        while (fast_frames.load() < kFastFrames && idle < 50) {
            std::string payload; int op = 0;
            int rc = fast.read_frame(payload, op);
            if (rc == 0) { ++idle; continue; }
            if (rc < 0) { fast_bad = true; break; }
            idle = 0;
            if (op != 0x1) continue;
            if (payload.compare(0, 5, "fast|") != 0) fast_bad = true;
            ++fast_frames;
        }
    });

    const size_t rss_before = rss_bytes();
    std::atomic<size_t> rss_peak{rss_before};
    std::atomic<bool> sampling{true};
    std::thread sampler([&] {
        while (sampling.load()) {
            size_t r = rss_bytes();
            size_t p = rss_peak.load();
            while (r > p && !rss_peak.compare_exchange_weak(p, r)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Push 256 MiB at the stalled client (64 KiB x 4096) while trickling
    // small frames to the healthy one.
    const std::string big(64 * 1024, 'S');
    const int kBigFrames = 4096;
    size_t accepted_slow = 0;
    int fast_sent = 0;
    for (int i = 0; i < kBigFrames; ++i) {
        accepted_slow += WsPushHub::Instance().BroadcastToUserCounted("slow", big);
        if (fast_sent < kFastFrames && (i % 2) == 0) {
            WsPushHub::Instance().BroadcastToUser("fast", "fast|" + std::to_string(fast_sent++));
        }
    }
    while (fast_sent < kFastFrames) {
        WsPushHub::Instance().BroadcastToUser("fast", "fast|" + std::to_string(fast_sent++));
        if ((fast_sent & 127) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // The slow consumer must be closed and unregistered.
    for (int t = 0; t < 500 && WsPushHub::Instance().UserConnectionCount("slow") != 0; ++t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fast_reader.join();
    sampling = false;
    sampler.join();

    myframe::OutboundStats st = myframe::outbound_stats();
    const size_t pushed = big.size() * static_cast<size_t>(kBigFrames);
    const size_t growth = rss_peak.load() > rss_before ? rss_peak.load() - rss_before : 0;

    CHECK(WsPushHub::Instance().UserConnectionCount("slow") == 0);
    CHECK(st.ws_slow_consumer_closed == 1);
    CHECK(st.ws_dropped_messages > 0);
    CHECK(st.ws_dropped_bytes > 0);
    // Only a bounded prefix (socket buffers + HWM) was accepted for the slow client.
    CHECK(accepted_slow * big.size() < pushed / 8);
    // RSS must stay bounded far below what an unbounded queue would need.
    CHECK(growth < 64u * 1024u * 1024u);

    // Healthy client is unaffected.
    CHECK(!fast_bad.load());
    CHECK(fast_frames.load() == kFastFrames);
    CHECK(WsPushHub::Instance().UserConnectionCount("fast") == 1);

    // The stalled socket is closed by the server: draining it ends in EOF/RST.
    {
        slow.set_timeout(2000);
        bool saw_close = false;
        for (int i = 0; i < 100000; ++i) {
            std::string payload; int op = 0;
            int rc = slow.read_frame(payload, op);
            if (rc < 0) { saw_close = true; break; }
            if (rc == 0) break;
        }
        CHECK(saw_close);
    }

    fast.close_fd();
    srv.stop();
    srv.join();

    std::printf("ws_slow_consumer: hwm=%zu pushed=%zu accepted_slow_frames=%zu rss_growth=%zu "
                "slow_closed=%llu dropped_msgs=%llu dropped_bytes=%llu fast_frames=%d -> %s\n",
                kHwm, pushed, accepted_slow, growth,
                (unsigned long long)st.ws_slow_consumer_closed,
                (unsigned long long)st.ws_dropped_messages,
                (unsigned long long)st.ws_dropped_bytes,
                fast_frames.load(), g_fail.load() ? "FAIL" : "PASS");
    return g_fail.load() ? 1 : 0;
}
