// Regression tests for listener fairness, server lifecycle isolation, and
// channel wakeup/FD teardown. Each case runs as its own CTest entry.

#include "base_net_thread.h"
#include "channel_data_process.h"
#include "factory_base.h"
#include "listen_factory.h"
#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n",         \
                         __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

// Counts connections from both the direct and the worker-dispatch paths.
class CountingFactory : public IFactory {
public:
    std::atomic<int> accepted{0};

    void on_accept(base_net_thread*, int fd) override { take(fd); }

    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override {
        if (!msg || msg->_msg_op != NORMAL_MSG_CONNECT) return;
        take(std::static_pointer_cast<content_msg>(msg)->fd);
    }

private:
    void take(int fd) {
        ::close(fd);
        accepted.fetch_add(1);
    }
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

unsigned short pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        ::getsockname(fd, (sockaddr*)&addr, &len) != 0) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

unsigned short bound_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, (sockaddr*)&addr, &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

int connect_loopback(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int count_open_fds() {
    int count = 0;
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) return -1;
    while (dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.') ++count;
    }
    ::closedir(dir);
    return count - 1;  // Exclude the descriptor used by opendir itself.
}

int pending_bytes(int fd) {
    int n = -1;
    ::ioctl(fd, FIONREAD, &n);
    return n;
}

void drain_bytes(int fd) {
    char buf[64];
    while (::recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

struct ServerHandle {
    std::shared_ptr<CountingFactory> biz = std::make_shared<CountingFactory>();
    unsigned short port = pick_free_port();
    server srv;

    explicit ServerHandle(int threads) : srv(threads) {
        srv.set_business_factory(std::make_shared<ListenFactory>("127.0.0.1", port, biz));
        srv.start();
    }

    // Returns true when a fresh connection is accepted and dispatched.
    bool serves_connection() {
        int before = biz->accepted.load();
        int fd = connect_loopback(port);
        if (fd < 0) return false;
        bool ok = wait_until([&] { return biz->accepted.load() > before; }, 3000);
        ::close(fd);
        return ok;
    }
};

// All connections are queued before the loop starts, so one wake must not strand
// everything past the 128-accept budget.
void test_accept_backlog() {
    const int kConnections = 200;
    CountingFactory factory;
    std::vector<int> clients;
    {
        base_net_thread th(&factory);
        std::shared_ptr<listen_connect<listen_process>> conn;
        listen_process* proc = nullptr;
        int listen_fd = ListenFactory::init_listen(&th, "127.0.0.1", 0, conn, proc);
        unsigned short port = bound_port(listen_fd);
        CHECK(port != 0);

        for (int i = 0; i < kConnections; ++i) {
            int fd = connect_loopback(port);
            CHECK(fd >= 0);
            if (fd >= 0) clients.push_back(fd);
        }
        CHECK(th.start());
        CHECK(wait_until([&] { return factory.accepted.load() == kConnections; }, 5000));
        std::fprintf(stderr, "accepted %d/%d\n", factory.accepted.load(), kConnections);
        th.stop();
        th.join_thread();
        conn.reset();
    }
    for (int fd : clients) ::close(fd);
}

void test_accept_burst() {
    const int kThreads = 8;
    const int kPerThread = 25;
    ServerHandle s(3);
    std::atomic<int> connected{0};
    std::vector<std::thread> clients;
    std::vector<std::vector<int>> fds(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                int fd = connect_loopback(s.port);
                if (fd >= 0) {
                    fds[t].push_back(fd);
                    connected.fetch_add(1);
                }
            }
        });
    }
    for (auto& c : clients) c.join();
    CHECK(connected.load() == kThreads * kPerThread);
    CHECK(wait_until([&] { return s.biz->accepted.load() == connected.load(); }, 5000));
    for (auto& v : fds) for (int fd : v) ::close(fd);
    s.srv.stop();
    s.srv.join();
}

// FD exhaustion must back off instead of spinning, then recover.
void test_accept_emfile() {
    CountingFactory factory;
    base_net_thread th(&factory);
    std::shared_ptr<listen_connect<listen_process>> conn;
    listen_process* proc = nullptr;
    int listen_fd = ListenFactory::init_listen(&th, "127.0.0.1", 0, conn, proc);
    unsigned short port = bound_port(listen_fd);
    CHECK(th.start());

    // Prime the control-block vptr checks used by the EMFILE retry timer.
    // A cold UBSan vptr check probes memory with pipe2(), which itself fails
    // when this test exhausts descriptors and falsely reports an invalid vptr.
    // Keep all sanitizer checks enabled and exercise the full control-block
    // lifecycle before exhausting FDs. The actual accept/backoff/recovery path
    // below still runs with no descriptors available.
    {
        std::shared_ptr<timer_msg> warmup(new timer_msg);
        std::shared_ptr<timer_msg> copy = warmup;
        copy.reset();
        warmup.reset();
    }

    rlimit original{};
    CHECK(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    rlimit lowered = original;
    lowered.rlim_cur = static_cast<rlim_t>(count_open_fds() + 32);
    CHECK(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);

    std::vector<int> fillers;
    for (;;) {
        int fd = ::dup(0);
        if (fd < 0) break;
        fillers.push_back(fd);
    }
    ::close(fillers.back());
    fillers.pop_back();
    int client = connect_loopback(port);  // Takes the last free descriptor.
    CHECK(client >= 0);

    rusage before{}, after{};
    ::getrusage(RUSAGE_SELF, &before);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ::getrusage(RUSAGE_SELF, &after);
    auto to_ms = [](const timeval& tv) { return tv.tv_sec * 1000L + tv.tv_usec / 1000L; };
    long cpu_ms = (to_ms(after.ru_utime) - to_ms(before.ru_utime)) +
                  (to_ms(after.ru_stime) - to_ms(before.ru_stime));
    std::fprintf(stderr, "cpu during EMFILE: %ld ms of 600 ms\n", cpu_ms);
    CHECK(cpu_ms < 200);
    CHECK(factory.accepted.load() == 0);

    for (int fd : fillers) ::close(fd);
    CHECK(::setrlimit(RLIMIT_NOFILE, &original) == 0);
    CHECK(wait_until([&] { return factory.accepted.load() == 1; }, 3000));

    if (client >= 0) ::close(client);
    th.stop();
    th.join_thread();
    conn.reset();
}

void test_server_isolation() {
    ServerHandle a(3);
    ServerHandle b(3);
    CHECK(a.serves_connection());
    CHECK(b.serves_connection());

    a.srv.stop();
    a.srv.join();

    CHECK(!a.srv.listen_thread()->get_run_flag());
    for (auto* w : a.srv.worker_threads()) CHECK(!w->get_run_flag());
    CHECK(b.srv.listen_thread()->get_run_flag());
    for (auto* w : b.srv.worker_threads()) CHECK(w->get_run_flag());

    // Cover every worker of the surviving server through round-robin dispatch.
    for (int i = 0; i < 8; ++i) CHECK(b.serves_connection());

    b.srv.stop();
    b.srv.join();
}

void test_idempotent_stop_join() {
    {
        server never_started(2);
        never_started.stop();
        never_started.join();
        never_started.join();
    }
    {
        ServerHandle s(3);
        CHECK(s.serves_connection());
        s.srv.stop();
        s.srv.stop();

        std::vector<std::thread> joiners;
        for (int i = 0; i < 4; ++i) joiners.emplace_back([&] { s.srv.join(); });
        for (auto& j : joiners) j.join();

        s.srv.join();
        s.srv.stop();
        s.srv.join();
        CHECK(s.srv.listen_thread()->get_thread_id() == 0);
        for (auto* w : s.srv.worker_threads()) CHECK(w->get_thread_id() == 0);
        // ~server performs one more stop/join.
    }
    {
        base_net_thread th;
        CHECK(th.start());
        CHECK(!th.start());
        th.stop();
        th.join_thread();
        th.join_thread();
        CHECK(!th.start());  // start() remains single-shot after a join.
    }
}

void test_channel_coalescing() {
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    {
        channel_data_process channel(std::shared_ptr<base_net_obj>(), fds[1]);
        auto put = [&](int n) {
            for (int i = 0; i < n; ++i) {
                std::shared_ptr<normal_msg> msg = std::make_shared<normal_msg>(0);
                channel.put_msg(OBJ_ID_THREAD, msg);
            }
        };

        put(500);
        CHECK(pending_bytes(fds[0]) == 1);

        drain_bytes(fds[0]);
        channel.on_tick();
        CHECK(pending_bytes(fds[0]) == 0);
        put(1);
        CHECK(pending_bytes(fds[0]) == 1);  // The drain re-armed the wakeup.
        drain_bytes(fds[0]);
        channel.on_tick();

        // Leave one message in the current queue, then add one to the idle queue.
        // The drain that empties the current queue must still signal the other.
        put(1025);
        drain_bytes(fds[0]);
        channel.on_tick();
        CHECK(pending_bytes(fds[0]) == 1);
        drain_bytes(fds[0]);
        put(1);
        CHECK(pending_bytes(fds[0]) == 0);  // Coalesced onto the armed state.
        channel.on_tick();
        CHECK(pending_bytes(fds[0]) == 1);
        drain_bytes(fds[0]);
        channel.on_tick();
        CHECK(pending_bytes(fds[0]) == 0);
    }
    errno = 0;
    CHECK(::fcntl(fds[1], F_GETFD) == -1 && errno == EBADF);
    char byte;
    CHECK(::recv(fds[0], &byte, 1, MSG_DONTWAIT) == 0);  // Peer closed.
    ::close(fds[0]);
}

void test_channel_fd_teardown() {
    int before = count_open_fds();
    for (int i = 0; i < 25; ++i) {
        base_net_thread th(2);
        CHECK(th.start());
        ObjId id;
        id._id = OBJ_ID_THREAD;
        id._thread_index = th.get_thread_index();
        std::shared_ptr<normal_msg> msg = std::make_shared<normal_msg>(0);
        base_net_thread::put_obj_msg(id, msg);
        th.stop();
        th.join_thread();
    }
    int after = count_open_fds();
    std::fprintf(stderr, "open fds before=%d after=%d\n", before, after);
    CHECK(before == after);
}

}  // namespace

int main(int argc, char** argv) {
    const std::map<std::string, void (*)()> cases = {
        {"accept_backlog", test_accept_backlog},
        {"accept_burst", test_accept_burst},
        {"accept_emfile", test_accept_emfile},
        {"server_isolation", test_server_isolation},
        {"idempotent_stop_join", test_idempotent_stop_join},
        {"channel_coalescing", test_channel_coalescing},
        {"channel_fd_teardown", test_channel_fd_teardown},
    };
    if (argc == 2) {
        auto it = cases.find(argv[1]);
        if (it == cases.end()) {
            std::fprintf(stderr, "unknown case: %s\n", argv[1]);
            return 2;
        }
        it->second();
    } else {
        for (const auto& c : cases) c.second();
    }
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
