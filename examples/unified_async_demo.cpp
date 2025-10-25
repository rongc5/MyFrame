// MyFrame Unified Protocol - Async response demo (ASCII version)
// Demonstrates using Level 1 and Level 2 handlers with async HTTP replies.

#include "server.h"
#include "unified_protocol_factory.h"
#include "protocol_context.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

class AsyncResponseHandler : public myframe::IProtocolHandler {
public:
    void on_http_request(myframe::HttpContext& ctx) override {
        const auto& req = ctx.request();
        std::cout << "[HTTP] " << req.method << " " << req.url << std::endl;

        if (req.url == "/async") {
            handle_async_request(ctx);
        } else if (req.url == "/sync") {
            ctx.response().set_json(R"({"type":"sync","data":"immediate response"})");
        } else {
            ctx.response().status = 404;
            ctx.response().set_text("Not Found");
        }
    }

    void handle_async_request(myframe::HttpContext& ctx) {
        const auto& info = ctx.connection_info();
        const uint64_t req_id = info.connection_id;
        std::cout << "[ASYNC] start request " << req_id << std::endl;

        ctx.async_response([&ctx, req_id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "[ASYNC] finish request " << req_id << std::endl;
            auto payload = std::string("{\"type\":\"async\",\"request_id\":") +
                           std::to_string(req_id) + "}";
            // 将结果封装在 HttpContextTaskMessage 中，回到网络线程再操作上下文
            auto task = std::make_shared<myframe::HttpContextTaskMessage>(
                [payload](myframe::HttpContext& context) {
                    context.response().set_json(payload);
                    context.complete_async_response();
                });
            ctx.send_msg(task);
        });
    }

    void handle_msg(std::shared_ptr<normal_msg>& msg) override {
        if (msg) {
            std::cout << "[MSG] op=" << msg->_msg_op << std::endl;
        }
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (t_msg) {
            std::cout << "[TIMEOUT] timer fired" << std::endl;
        }
    }

    void on_connect(const myframe::ConnectionInfo& info) override {
        std::cout << "[CONNECT] from " << info.remote_ip << ':' << info.remote_port
                  << " id=" << info.connection_id << std::endl;
    }

    void on_disconnect() override {
        std::cout << "[DISCONNECT]" << std::endl;
    }
};

class SimpleAsyncHandler : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req,
                 myframe::HttpResponse& res) override {
        int count = ++request_count_;
        std::cout << "[HTTP L1 #" << count << "] " << req.method
                  << " " << req.url << std::endl;

        if (req.url == "/") {
            res.set_html(
                "<!DOCTYPE html>\n"
                "<html><head><title>Async Demo</title></head><body>\n"
                "<h1>MyFrame Async Demo</h1>\n"
                "<ul>\n"
                "  <li><a href=\"/sync\">Sync response</a></li>\n"
                "  <li><a href=\"/async\">Async response</a></li>\n"
                "</ul>\n"
                "</body></html>\n");
        } else if (req.url == "/sync") {
            res.set_json(std::string("{\"type\":\"sync\",\"count\":") +
                         std::to_string(count) + "}");
        } else {
            res.status = 404;
            res.set_text("Not Found");
        }
    }

    void handle_msg(std::shared_ptr<normal_msg>& msg) override {
        if (msg) {
            std::cout << "[MSG L1] op=" << msg->_msg_op << std::endl;
        }
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (t_msg) {
            std::cout << "[TIMEOUT L1] timer fired" << std::endl;
        }
    }

private:
    std::atomic<int> request_count_{0};
};

int main(int argc, char* argv[]) {
    unsigned short port = 8080;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    std::cout << "MyFrame Async Demo" << std::endl;
    std::cout << "Listening on 0.0.0.0:" << port << std::endl;
    std::cout << "Endpoints:" << std::endl;
    std::cout << "  /      -> HTML page" << std::endl;
    std::cout << "  /sync  -> synchronous JSON" << std::endl;
    std::cout << "  /async -> asynchronous JSON" << std::endl;

    try {
        ::server srv(2);
        auto factory = std::make_shared<myframe::UnifiedProtocolFactory>();

        auto level2_handler = std::make_shared<AsyncResponseHandler>();
        auto level1_handler = std::make_shared<SimpleAsyncHandler>();

        const bool use_level2 = true;
        if (use_level2) {
            std::cout << "Using Level 2 protocol handler" << std::endl;
            factory->register_http_context_handler(level2_handler.get());
        } else {
            std::cout << "Using Level 1 application handler" << std::endl;
            factory->register_http_handler(level1_handler.get());
        }

        srv.bind("0.0.0.0", port);
        srv.set_business_factory(factory);
        srv.start();
        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
