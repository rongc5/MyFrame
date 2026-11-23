// P1 regression tests: monotonic single-map timer, lock-free put_obj_msg under
// load and concurrent teardown, socketpair-failure safety, stop() wakeup, and
// the graceful shutdown drain.
#include "base_connect.h"
#include "base_data_process.h"
#include "base_net_thread.h"
#include "base_timer.h"
#include "common_util.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const int kTokenOp = 9101;

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct TokenMsg : public normal_msg {
    explicit TokenMsg(uint64_t t) : normal_msg(kTokenOp), token(t) {}
    uint64_t token;
};

// Counts token messages. Unpublishes first so concurrent posters never make a
// virtual call while this derived object is being destroyed.
class CountingThread : public base_net_thread {
public:
    CountingThread(std::atomic<uint64_t>& count, std::atomic<uint64_t>& checksum)
        : base_net_thread(1), count_(count), checksum_(checksum) {}
    ~CountingThread() override { unpublish_thread(); }

protected:
    bool handle_thread_msg(std::shared_ptr<normal_msg>& msg) override {
        if (msg && msg->_msg_op == kTokenOp) {
            checksum_.fetch_xor(static_cast<TokenMsg*>(msg.get())->token, std::memory_order_relaxed);
            count_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }

private:
    std::atomic<uint64_t>& count_;
    std::atomic<uint64_t>& checksum_;
};

bool post_token(uint32_t thread_index, uint64_t token) {
    ObjId id;
    id._id = OBJ_ID_THREAD;
    id._thread_index = thread_index;
    std::shared_ptr<normal_msg> msg = std::make_shared<TokenMsg>(token);
    return base_net_thread::put_obj_msg(id, msg);
}

class TimerThread : public base_net_thread {
public:
    TimerThread() : base_net_thread(1) {}
    void handle_timeout(std::shared_ptr<timer_msg>& t) override {
        ++fired;
        if (rearm > 0) {
            --rearm;
            // Re-entrant add from inside the callback, reusing the same message.
            add_timer(t);
        }
    }
    int fired{0};
    int rearm{0};
};

void arm(TimerThread& t, uint32_t ms) {
    std::shared_ptr<timer_msg> m(new timer_msg);
    m->_obj_id = OBJ_ID_THREAD;
    m->_timer_type = 1;
    m->_time_length = ms;
    t.add_timer(m);
}

void test_monotonic_timer() {
    uint64_t prev = GetMonotonicMilliSecond();
    for (int i = 0; i < 10000; ++i) {
        uint64_t cur = GetMonotonicMilliSecond();
        assert(cur >= prev);
        prev = cur;
    }

    TimerThread t;
    base_timer* timer = t.get_net_container()->get_timer();
    std::vector<uint32_t> failed;
    assert(timer->is_empty());

    arm(t, 1);
    arm(t, 1);
    arm(t, 60000);  // must stay queued
    t.rearm = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // One pass fires each expired entry once; the re-armed timer waits.
    timer->check_timer(failed);
    assert(t.fired == 2);
    assert(!timer->is_empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    timer->check_timer(failed);
    assert(t.fired == 3);
    timer->check_timer(failed);
    assert(t.fired == 3);
    assert(!timer->is_empty());  // the long timer remains
    assert(failed.empty());
    std::cout << "[ OK ] monotonic_single_map_timer fired=" << t.fired << std::endl;
}

void test_no_channel_safety() {
    base_net_thread no_channels(0);
    std::shared_ptr<normal_msg> msg = std::make_shared<TokenMsg>(1);
    no_channels.put_msg(OBJ_ID_THREAD, msg);  // must not divide by zero
    assert(!post_token(no_channels.get_thread_index(), 1));
    assert(!post_token(0xFFFFFFF0u, 1));  // unknown thread, slow path
    std::cout << "[ OK ] no_channel_safety" << std::endl;
}

void test_producer_stress() {
    const int kProducers = 16;
    const int kWorkers = 4;
    const int kPerProducer = 25000;
    const uint64_t kTotal = static_cast<uint64_t>(kProducers) * kPerProducer;

    std::atomic<uint64_t> count{0}, recv_xor{0}, sent_xor{0}, rejected{0};
    std::vector<std::unique_ptr<CountingThread>> workers;
    std::vector<uint32_t> ids;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back(new CountingThread(count, recv_xor));
        workers.back()->start();
        ids.push_back(workers.back()->get_thread_index());
    }

    auto begin = std::chrono::steady_clock::now();
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            uint64_t local = 0;
            for (int s = 0; s < kPerProducer; ++s) {
                uint64_t token = mix64((static_cast<uint64_t>(p) << 32) | static_cast<uint32_t>(s));
                if (post_token(ids[(p + s) % kWorkers], token)) local ^= token;
                else rejected.fetch_add(1, std::memory_order_relaxed);
            }
            sent_xor.fetch_xor(local, std::memory_order_relaxed);
        });
    }
    for (auto& th : producers) th.join();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    while (count.load(std::memory_order_acquire) < kTotal && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    for (auto& w : workers) w->stop();
    for (auto& w : workers) w->join_thread();

    std::cout << "  producers=" << kProducers << " received=" << count.load() << "/" << kTotal
              << " rejected=" << rejected.load()
              << " xor_match=" << (recv_xor.load() == sent_xor.load() ? "YES" : "NO")
              << " elapsed_ms=" << ms << std::endl;
    assert(rejected.load() == 0);
    assert(count.load() == kTotal);
    assert(recv_xor.load() == sent_xor.load());
    std::cout << "[ OK ] producer_stress" << std::endl;
}

void test_concurrent_teardown() {
    const int kRounds = 30;
    const int kSlots = 4;
    std::atomic<uint64_t> handled{0}, checksum{0}, accepted{0}, refused{0};
    std::atomic<uint32_t> targets[kSlots];
    for (auto& t : targets) t.store(0xFFFFFFF0u);
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&, p]() {
            uint64_t n = 0;
            while (!done.load(std::memory_order_acquire)) {
                uint32_t idx = targets[(p + n) % kSlots].load(std::memory_order_acquire);
                if (post_token(idx, mix64(n))) accepted.fetch_add(1, std::memory_order_relaxed);
                else refused.fetch_add(1, std::memory_order_relaxed);
                if (++n % 32 == 0) std::this_thread::yield();
            }
        });
    }

    for (int r = 0; r < kRounds; ++r) {
        std::vector<CountingThread*> round;
        for (int i = 0; i < kSlots; ++i) {
            round.push_back(new CountingThread(handled, checksum));
            round.back()->start();
            targets[i].store(round.back()->get_thread_index(), std::memory_order_release);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::vector<std::thread> killers;
        for (auto* w : round) {
            killers.emplace_back([w]() {
                w->stop();
                w->join_thread();
                delete w;  // targets[] still names it: posters must get false
            });
        }
        for (auto& k : killers) k.join();
    }
    done.store(true, std::memory_order_release);
    for (auto& p : producers) p.join();

    std::cout << "  rounds=" << kRounds << " accepted=" << accepted.load() << " handled=" << handled.load()
              << " refused=" << refused.load() << std::endl;
    assert(accepted.load() > 0);
    assert(refused.load() > 0);
    assert(handled.load() <= accepted.load());
    std::cout << "[ OK ] concurrent_teardown" << std::endl;
}

void test_stop_wakes_sleeping_loop() {
    // A long epoll timeout makes a missed wakeup obvious.
    setenv("MYFRAME_EPOLL_WAIT_MS", "1000", 1);
    std::atomic<uint64_t> count{0}, checksum{0};
    {
        CountingThread t(count, checksum);
        unsetenv("MYFRAME_EPOLL_WAIT_MS");
        t.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        auto begin = std::chrono::steady_clock::now();
        t.stop();
        t.join_thread();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        std::cout << "  stop_to_exit_ms=" << ms << std::endl;
        assert(ms < 500);
    }
    std::cout << "[ OK ] stop_wakes_sleeping_loop" << std::endl;
}

void test_drain_queued_messages() {
    const uint64_t kMessages = 3000;
    std::atomic<uint64_t> count{0}, recv_xor{0};
    uint64_t sent_xor = 0;
    {
        CountingThread t(count, recv_xor);
        t.start();
        for (uint64_t i = 0; i < kMessages; ++i) {
            uint64_t token = mix64(i);
            bool ok = post_token(t.get_thread_index(), token);
            assert(ok);
            sent_xor ^= token;
        }
        t.stop();
        t.join_thread();
        // The loop has exited: later posts are refused rather than stranded.
        assert(!post_token(t.get_thread_index(), 1));
        assert(count.load() == kMessages);
        assert(recv_xor.load() == sent_xor);
    }
    std::cout << "[ OK ] drain_queued_messages delivered=" << count.load() << "/" << kMessages << std::endl;
}

class SinkProcess : public base_data_process {
public:
    explicit SinkProcess(std::shared_ptr<base_net_obj> p) : base_data_process(p) {}
    size_t process_recv_buf(const char*, size_t len) override { return len; }
};

void test_drain_flushes_outbound() {
    const size_t kPayload = 2u * 1024u * 1024u;
    std::string payload(kPayload, '\0');
    for (size_t i = 0; i < kPayload; ++i) payload[i] = static_cast<char>(i * 31);

    int fds[2];
    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);
    int sndbuf = 16 * 1024;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    base_net_thread* t = new base_net_thread(1);
    {
        std::shared_ptr<base_connect<SinkProcess>> conn(new base_connect<SinkProcess>(fds[0]));
        SinkProcess* proc = new SinkProcess(conn);
        conn->set_process(proc);
        conn->set_net_container(t->get_net_container());
        proc->put_send_copy(payload);  // fills the socket; the rest stays queued
        assert(conn->wants_tick());
    }

    t->start();
    t->stop();  // the peer only starts reading after stop()

    std::string received;
    received.reserve(kPayload);
    std::thread reader([&]() {
        char buf[64 * 1024];
        for (;;) {
            ssize_t n = ::read(fds[1], buf, sizeof(buf));
            if (n <= 0) break;
            received.append(buf, static_cast<size_t>(n));
        }
    });
    t->join_thread();
    delete t;  // closes the connection, so the reader sees EOF
    reader.join();
    ::close(fds[1]);

    std::cout << "  flushed_bytes=" << received.size() << "/" << kPayload << std::endl;
    assert(received == payload);
    std::cout << "[ OK ] drain_flushes_outbound" << std::endl;
}

}  // namespace

int main() {
    test_monotonic_timer();
    test_no_channel_safety();
    test_stop_wakes_sleeping_loop();
    test_drain_queued_messages();
    test_drain_flushes_outbound();
    test_producer_stress();
    test_concurrent_teardown();
    std::cout << "ALL P1 CORE TESTS PASSED!" << std::endl;
    return 0;
}
