#include "../core/server.h"
#include "../core/listen_factory.h"
#include "../core/factory_base.h"
#include "../core/common_obj_container.h"
#include "../core/base_net_thread.h"
#include "../core/base_connect.h"
#include "../core/http_res_process.h"
#include "../core/app_handler.h"
#include "../core/app_http_data_process.h"
#include <iostream>
#include <memory>

// 1) Business handler: generate a simple response
class CloseDemoHandler : public IAppHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& rsp) override {
        rsp.status = 200; rsp.reason = "OK";
        rsp.set_header("Content-Type", "text/plain; charset=utf-8");
        rsp.body = std::string("Close-Demo\nMethod: ") + req.method + "\nPath: " + req.url + "\n";
    }
    void on_ws(const WsFrame&, WsFrame& send) override {
        send = WsFrame::text("close-demo server does not support ws");
    }
};

// 2) Data process: after sending response, gracefully close current connection
class CloseAfterSendDP : public app_http_data_process {
public:
    using app_http_data_process::app_http_data_process;
    void msg_recv_finish() override {
        app_http_data_process::msg_recv_finish();
        // Send head/body as usual; then schedule a very short delayed close
        request_close_after(10);
    }
};

// 3) Simple HTTP-only factory using CloseAfterSendDP (no TLS/WS here)
class HttpCloseFactory : public IFactory {
public:
    explicit HttpCloseFactory(IAppHandler* h) : _handler(h) {}
    void net_thread_init(base_net_thread* th) override { _container = th ? th->get_net_container() : nullptr; }
    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override {
        if (!msg || msg->_msg_op != NORMAL_MSG_CONNECT || !_container) return;
        std::shared_ptr<content_msg> cm = std::static_pointer_cast<content_msg>(msg);
        int fd = cm->fd;
        std::shared_ptr< base_connect<base_data_process> > conn(new base_connect<base_data_process>(fd));
        std::unique_ptr<http_res_process> p(new http_res_process(conn));
        p->set_process(new CloseAfterSendDP(p.get(), _handler));
        conn->set_process(p.release());
        conn->set_net_container(_container);
        std::shared_ptr<base_net_obj> net_obj = conn;
        _container->push_real_net(net_obj);
    }
    void register_worker(uint32_t) override {}
    void on_accept(base_net_thread*, int) override {}
private:
    IAppHandler* _handler{nullptr};
    common_obj_container* _container{nullptr};
};

int main(int argc, char** argv) {
    unsigned short port = 8088;
    if (argc > 1) port = (unsigned short)atoi(argv[1]);
    std::cout << "=== HTTP Close Demo Server ===\nPort: " << port << std::endl;

    CloseDemoHandler handler;
    std::shared_ptr<IFactory> http_factory(new HttpCloseFactory(&handler));
    std::shared_ptr<ListenFactory> lsn(new ListenFactory("127.0.0.1", port, http_factory));

    server srv(2); // 1 listen + 1 worker
    srv.set_business_factory(lsn);
    srv.start();
    srv.join();
    return 0;
}

