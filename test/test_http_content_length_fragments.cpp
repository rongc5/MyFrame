#include "http_base_data_process.h"
#include "http_req_process.h"
#include "http_res_process.h"

#include <iostream>
#include <memory>
#include <string>

namespace {

class EagerChunkConsumer : public http_base_data_process {
public:
    explicit EagerChunkConsumer(http_base_process* owner)
        : http_base_data_process(owner) {}

    size_t process_recv_body(const char* buf, size_t len, int& result) override {
        if (len > 0) body.append(buf, len);
        result = 1;  // A consumed chunk must not finish a framed HTTP message.
        return len;
    }

    void msg_recv_finish() override { ++finishes; }

    std::string body;
    int finishes{0};
};

class ProbeServer : public http_res_process {
public:
    ProbeServer() : http_res_process(std::shared_ptr<base_net_obj>()) {
        consumer = new EagerChunkConsumer(this);
        set_process(consumer);
    }

    EagerChunkConsumer* consumer;
};

class ProbeClient : public http_req_process {
public:
    ProbeClient() : http_req_process(std::shared_ptr<base_net_obj>()) {
        consumer = new EagerChunkConsumer(this);
        set_process(consumer);
        change_http_status(RECV_HEAD, false);
    }

    EagerChunkConsumer* consumer;
};

template <typename Process>
bool check_fragments(Process& process, const std::string& header) {
    const std::string body = "hello=world";
    process.process_recv_buf(header.data(), header.size());
    if (process.consumer->finishes != 0 || !process.consumer->body.empty()) return false;

    process.process_recv_buf(body.data(), 5);
    if (process.consumer->finishes != 0 || process.consumer->body != "hello") return false;

    process.process_recv_buf(body.data() + 5, body.size() - 5);
    return process.consumer->finishes == 1 && process.consumer->body == body;
}

}  // namespace

int main() {
    ProbeServer server;
    if (!check_fragments(server,
            "POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n")) {
        std::cerr << "server finished a fragmented request body early\n";
        return 1;
    }

    // Adjacent bodyless requests must not be swallowed as a GET/HEAD body.
    for (const std::string method : {"GET", "HEAD"}) {
        ProbeServer pipeline;
        const std::string first = method + " /first HTTP/1.1\r\nHost: localhost\r\n\r\n";
        const std::string second = "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";
        const std::string wire = first + second;
        const auto consumed = pipeline.process_recv_buf(wire.data(), wire.size());
        if (consumed != first.size() || pipeline.consumer->finishes != 1) return 1;
        pipeline.reset();
        if (pipeline.process_recv_buf(wire.data() + consumed, wire.size() - consumed) != second.size() ||
            pipeline.consumer->finishes != 2) return 1;
        ProbeServer body;
        if (!check_fragments(body, method + " /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n")) return 1;
    }

    ProbeClient client;
    if (!check_fragments(client,
            "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n")) {
        std::cerr << "client finished a fragmented response body early\n";
        return 1;
    }

    return 0;
}
