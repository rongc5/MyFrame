// High-concurrency stress & exact connection/message accounting test for myframe.
#include "server.h"
#include "unified_protocol_factory.h"
#include "http_router.h"
#include "http_routing_handler.h"
#include "http_middleware.h"
#include "base_net_thread.h"
#include "base_thread.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

int count_open_fds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d) != nullptr) ++n;
    closedir(d);
    return n - 2;
}

unsigned short pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    unsigned short port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool recv_http_response(int fd, std::string& buf, std::string& out_body) {
    while (true) {
        size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            std::string hdr = buf.substr(0, hdr_end);
            size_t cl_pos = hdr.find("Content-Length:");
            if (cl_pos == std::string::npos) cl_pos = hdr.find("content-length:");
            if (cl_pos == std::string::npos) return false;
            size_t val_start = cl_pos + 15;
            while (val_start < hdr.size() && hdr[val_start] == ' ') ++val_start;
            size_t cl = static_cast<size_t>(std::stoul(hdr.substr(val_start)));
            size_t total = hdr_end + 4 + cl;
            if (buf.size() >= total) {
                out_body = buf.substr(hdr_end + 4, cl);
                buf.erase(0, total);
                return hdr.find("200") != std::string::npos;
            }
        }
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
}

void test_high_concurrency_http_accounting() {
    std::cout << "[RUN ] high_concurrency_http_accounting (400 burst conns x 20 reqs = 8000 HTTP msgs)" << std::endl;
    const int kWorkers = 8;
    const int kClients = 400;
    const int kReqsPerClient = 20;
    const int kExpectedTotal = kClients * kReqsPerClient;

    const int fd_before = count_open_fds();
    const unsigned short port = pick_free_port();

    std::atomic<int> server_reqs{0};
    std::mutex tid_mu;
    std::unordered_map<std::thread::id, int> per_worker_reqs;

    myframe::Router router;
    router.get("/echo/{cid}/{seq}", [&](myframe::HttpContext& c) {
        server_reqs.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(tid_mu);
            per_worker_reqs[std::this_thread::get_id()]++;
        }
        const std::string cid = c.route_param("cid");
        const std::string seq = c.route_param("seq");
        const std::string conn_hdr = c.request().get_header("Connection");
        c.keep_alive(conn_hdr == "keep-alive" || conn_hdr == "Keep-Alive");
        c.response().status = 200;
        c.response().set_json("{\"c\":\"" + cid + "\",\"s\":\"" + seq + "\"}");
    });
    router.freeze();

    myframe::MiddlewareChain chain;
    myframe::RoutingHandler handler(router, chain);
    auto factory = std::make_shared<myframe::UnifiedProtocolFactory>();
    factory->register_http_context_handler(&handler);

    {
        server srv(kWorkers + 1);
        srv.bind("127.0.0.1", port);
        srv.set_business_factory(factory);
        srv.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        std::mutex start_mu;
        std::condition_variable start_cv;
        int ready = 0;
        bool go = false;

        std::atomic<int> connected_ok{0};
        std::atomic<int> verified_msgs{0};
        std::atomic<int> failures{0};

        std::vector<std::thread> clients;
        clients.reserve(kClients);
        for (int cid = 0; cid < kClients; ++cid) {
            clients.emplace_back([&, cid]() {
                {
                    std::unique_lock<std::mutex> lk(start_mu);
                    ++ready;
                    if (ready == kClients) start_cv.notify_all();
                    start_cv.wait(lk, [&]() { return go; });
                }
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) { failures.fetch_add(1); return; }
                timeval tv{};
                tv.tv_sec = 20;
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                    ::close(fd);
                    failures.fetch_add(1);
                    return;
                }
                connected_ok.fetch_add(1);

                std::string recv_buf;
                for (int seq = 0; seq < kReqsPerClient; ++seq) {
                    const bool last = (seq == kReqsPerClient - 1);
                    std::ostringstream req;
                    req << "GET /echo/" << cid << "/" << seq << " HTTP/1.1\r\n"
                        << "Host: 127.0.0.1\r\n"
                        << "Connection: " << (last ? "close" : "keep-alive") << "\r\n\r\n";
                    if (!send_all(fd, req.str())) {
                        failures.fetch_add(1);
                        break;
                    }
                    std::string body;
                    if (!recv_http_response(fd, recv_buf, body)) {
                        failures.fetch_add(1);
                        break;
                    }
                    const std::string expected = "{\"c\":\"" + std::to_string(cid) + "\",\"s\":\"" + std::to_string(seq) + "\"}";
                    if (body != expected) {
                        failures.fetch_add(1);
                        break;
                    }
                    verified_msgs.fetch_add(1);
                }
                ::close(fd);
            });
        }

        {
            std::unique_lock<std::mutex> lk(start_mu);
            start_cv.wait(lk, [&]() { return ready == kClients; });
            go = true;
            start_cv.notify_all();
        }
        for (auto& t : clients) t.join();

        srv.stop();
        srv.join();

        std::cout << "  connected=" << connected_ok.load() << "/" << kClients
                  << " server_reqs=" << server_reqs.load() << "/" << kExpectedTotal
                  << " verified_msgs=" << verified_msgs.load() << "/" << kExpectedTotal
                  << " failures=" << failures.load()
                  << " active_workers=" << per_worker_reqs.size() << "/" << kWorkers
                  << std::endl;

        assert(failures.load() == 0);
        assert(connected_ok.load() == kClients);
        assert(server_reqs.load() == kExpectedTotal);
        assert(verified_msgs.load() == kExpectedTotal);
        assert(per_worker_reqs.size() == static_cast<size_t>(kWorkers));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int fd_after = count_open_fds();
    std::cout << "  fds_before=" << fd_before << " fds_after=" << fd_after << std::endl;
    assert(fd_after == fd_before);
    std::cout << "[ OK ] high_concurrency_http_accounting" << std::endl;
}

struct AccountingMsg final : normal_msg {
    uint64_t producer;
    uint64_t seq;
    uint64_t token;
    AccountingMsg(uint64_t p, uint64_t s) : producer(p), seq(s), token((p << 32) ^ (s * 2654435761ULL)) {
        _msg_op = 9001;
    }
};

class CountingWorkerThread final : public base_net_thread {
public:
    CountingWorkerThread(std::atomic<uint64_t>& count, std::atomic<uint64_t>& checksum)
        : base_net_thread(1), count_(count), checksum_(checksum) {}
protected:
    bool handle_thread_msg(std::shared_ptr<normal_msg>& msg) override {
        if (msg && msg->_msg_op == 9001) {
            auto m = std::static_pointer_cast<AccountingMsg>(msg);
            checksum_.fetch_xor(m->token, std::memory_order_relaxed);
            count_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }
private:
    std::atomic<uint64_t>& count_;
    std::atomic<uint64_t>& checksum_;
};

void test_high_concurrency_queue_accounting() {
    std::cout << "[RUN ] high_concurrency_queue_accounting (8 producers -> 4 workers, 40000 msgs)" << std::endl;
    const int kWorkers = 4;
    const int kProducers = 8;
    const int kPerProducer = 5000;
    const uint64_t kTotal = static_cast<uint64_t>(kProducers) * kPerProducer;

    const int fd_before = count_open_fds();
    std::atomic<uint64_t> recv_count{0};
    std::atomic<uint64_t> recv_xor{0};
    std::atomic<uint64_t> sent_xor{0};

    {
        std::vector<std::unique_ptr<CountingWorkerThread>> workers;
        std::vector<uint32_t> tids;
        for (int i = 0; i < kWorkers; ++i) {
            workers.emplace_back(new CountingWorkerThread(recv_count, recv_xor));
            workers.back()->start();
            tids.push_back(workers.back()->get_thread_index());
        }

        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p]() {
                uint64_t local_xor = 0;
                for (int s = 0; s < kPerProducer; ++s) {
                    auto am = std::make_shared<AccountingMsg>(static_cast<uint64_t>(p), static_cast<uint64_t>(s));
                    local_xor ^= am->token;
                    std::shared_ptr<normal_msg> nm = am;
                    ObjId id;
                    id._id = OBJ_ID_THREAD;
                    id._thread_index = tids[(p + s) % kWorkers];
                    base_net_thread::put_obj_msg(id, nm);
                }
                sent_xor.fetch_xor(local_xor, std::memory_order_relaxed);
            });
        }
        for (auto& t : producers) t.join();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (recv_count.load(std::memory_order_acquire) < kTotal &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        for (auto& w : workers) w->stop();
        for (auto& w : workers) w->join_thread();

        std::cout << "  received=" << recv_count.load() << "/" << kTotal
                  << " xor_match=" << (recv_xor.load() == sent_xor.load() ? "YES" : "NO")
                  << std::endl;

        assert(recv_count.load() == kTotal);
        assert(recv_xor.load() == sent_xor.load());
    }

    const int fd_after = count_open_fds();
    std::cout << "  fds_before=" << fd_before << " fds_after=" << fd_after << std::endl;
    assert(fd_after == fd_before);
    std::cout << "[ OK ] high_concurrency_queue_accounting" << std::endl;
}

} // namespace

int main() {
    test_high_concurrency_http_accounting();
    test_high_concurrency_queue_accounting();
    std::cout << "ALL HIGH-CONCURRENCY STRESS TESTS PASSED!" << std::endl;
    return 0;
}
