/**
 * @file async_http_server_demo.cpp
 * @brief 演示异步 HTTP 响应功能
 *
 * 本示例展示如何使用框架的异步接口：
 * - 在 msg_recv_finish() 中设置 async_response_pending(true)
 * - 启动定时器模拟异步操作（如数据库查询、远程API调用）
 * - 在 handle_timeout() 中调用 complete_async_response() 完成响应
 */

#include "../core/server.h"
#include "../core/listen_factory.h"
#include "../core/http_res_process.h"
#include "../core/http_base_data_process.h"
#include "../core/factory_base.h"
#include "../core/base_net_thread.h"
#include "../core/common_obj_container.h"
#include "../core/base_connect.h"
#include "../core/common_def.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <atomic>

// 自定义定时器类型
enum AsyncTimerType {
    ASYNC_QUERY_TIMER = 100  // 异步查询定时器
};

/**
 * @brief 异步 HTTP 数据处理类
 *
 * 演示两种请求处理模式：
 * 1. /sync - 同步响应（立即返回）
 * 2. /async - 异步响应（模拟延迟处理）
 */
class async_http_data_process : public http_base_data_process
{
public:
    async_http_data_process(http_base_process* p)
        : http_base_data_process(p)
        , _head_ready(false)
        , _body_ready(false)
    {
    }

    virtual ~async_http_data_process() {}

    // 接收 HTTP body 数据
    virtual size_t process_recv_body(const char* buf, size_t len, int& result) override
    {
        if (buf && len) {
            _recv_body.append(buf, len);
        }
        result = 0;  // 由 http_res_process 根据 Content-Length 设置 result=1
        return len;
    }

    // HTTP 头部接收完成
    virtual void header_recv_finish() override
    {
        auto& req_head = _base_process->get_req_head_para();
        std::cout << "[Header] " << req_head._method << " " << req_head._url_path << std::endl;
    }

    // 消息接收完成 - 核心异步逻辑
    virtual void msg_recv_finish() override
    {
        auto& req_head = _base_process->get_req_head_para();
        std::string url = req_head._url_path;

        std::cout << "[Request] " << req_head._method << " " << url << std::endl;

        // 根据 URL 决定同步或异步处理
        if (url.find("/async") != std::string::npos)
        {
            // 异步模式：设置异步状态，启动定时器模拟异步操作
            std::cout << "[Async] Starting async operation for " << url << std::endl;

            // **关键步骤1**：设置异步标志，阻止 recv_finish() 立即发送响应
            set_async_response_pending(true);

            // 启动定时器模拟异步操作（1秒后触发）
            auto timer = std::make_shared<timer_msg>();
            timer->_timer_type = ASYNC_QUERY_TIMER;
            timer->_time_length = 1000;  // 1秒延迟

            // **重要**：必须设置 _obj_id，否则 add_timer 会失败
            if (auto conn = get_base_net()) {
                timer->_obj_id = conn->get_id()._id;
            }

            // 保存请求信息供稍后使用
            _async_url = url;

            add_timer(timer);

            std::cout << "[Async] Async flag set, timer started (1s delay)" << std::endl;
            // 注意：这里不生成响应，等待定时器触发
        }
        else
        {
            // 同步模式：立即生成响应
            std::cout << "[Sync] Processing sync request for " << url << std::endl;
            generate_response(url, false);
        }
    }

    // 定时器超时回调 - 异步操作完成
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override
    {
        if (t_msg->_timer_type == ASYNC_QUERY_TIMER)
        {
            std::cout << "[Async] Timer triggered, completing async operation" << std::endl;

            // 模拟异步操作完成，生成响应
            generate_response(_async_url, true);

            // **关键步骤2 & 3**：使用便利方法完成异步响应（清除标志 + 通知发送）
            complete_async_response();

            std::cout << "[Async] Response ready, complete_async_response() called" << std::endl;
        }
    }

    // 获取发送头
    virtual std::string* get_send_head() override
    {
        if (!_head_ready || !_p_send_head) {
            return nullptr;
        }
        _head_ready = false;
        return _p_send_head.release();
    }

    // 获取发送体
    virtual std::string* get_send_body(int& result) override
    {
        if (!_body_ready || !_p_send_body) {
            result = 1;
            return nullptr;
        }
        _body_ready = false;
        result = 1;
        return _p_send_body.release();
    }

private:
    /**
     * @brief 生成 HTTP 响应
     * @param url 请求的 URL
     * @param is_async 是否异步处理
     */
    void generate_response(const std::string& url, bool is_async)
    {
        // 构造响应体
        std::string body;
        body += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
        body += "<title>Async HTTP Demo</title></head><body>";
        body += "<h1>Async HTTP Response Demo</h1>";
        body += "<p><strong>URL:</strong> " + url + "</p>";
        body += "<p><strong>Mode:</strong> " + std::string(is_async ? "ASYNC (1s delay)" : "SYNC (immediate)") + "</p>";
        body += "<hr>";
        body += "<h2>Test Links:</h2>";
        body += "<ul>";
        body += "<li><a href='/sync'>Sync Request (immediate)</a></li>";
        body += "<li><a href='/async'>Async Request (1s delay)</a></li>";
        body += "<li><a href='/async/db'>Async DB Query (1s delay)</a></li>";
        body += "</ul>";

        if (is_async) {
            body += "<p style='color: green;'>✓ This response was processed asynchronously!</p>";
            body += "<p><em>The server set async_response_pending(true), waited 1 second, ";
            body += "then called complete_async_response() to flush the reply.</em></p>";
        } else {
            body += "<p style='color: blue;'>⚡ This response was processed synchronously!</p>";
            body += "<p><em>The server returned immediately without async flag.</em></p>";
        }

        body += "</body></html>";

        // 构造响应头
        _p_send_head.reset(new std::string);
        *_p_send_head = "HTTP/1.1 200 OK\r\n";
        *_p_send_head += "Content-Type: text/html; charset=utf-8\r\n";
        *_p_send_head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        *_p_send_head += "Connection: close\r\n";
        *_p_send_head += "Server: MyFrame-AsyncDemo/1.0\r\n";
        *_p_send_head += "\r\n";
        _head_ready = true;

        // 设置响应体
        _p_send_body.reset(new std::string);
        _p_send_body->swap(body);
        _body_ready = true;
    }

private:
    std::string _recv_body;          // 接收的请求体
    std::string _async_url;          // 保存异步请求的 URL

    std::unique_ptr<std::string> _p_send_head;  // 发送头缓冲
    std::unique_ptr<std::string> _p_send_body;  // 发送体缓冲
    bool _head_ready;
    bool _body_ready;
};

// 业务工厂：创建 http_res_process 和 async_http_data_process
class async_http_factory : public IFactory
{
public:
    async_http_factory() : _container(nullptr), _rr_hint(0) {}

    virtual ~async_http_factory() = default;

    // 线程初始化时获取容器
    virtual void net_thread_init(base_net_thread* th) override
    {
        if (!th) return;
        _container = th->get_net_container();
    }

    // 处理线程消息（接收新连接）
    virtual void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override
    {
        if (!msg || msg->_msg_op != NORMAL_MSG_CONNECT) return;

        if (!_container) {
            PDEBUG("%s", "[async_factory] container is null");
            return;
        }

        std::shared_ptr<content_msg> cm = std::static_pointer_cast<content_msg>(msg);
        int fd = cm->fd;

        PDEBUG("[async_factory] creating connection for fd=%d", fd);

        // 创建连接对象
        std::shared_ptr< base_connect<base_data_process> > conn(
            new base_connect<base_data_process>(fd));

        // 创建 http_res_process（负责 HTTP 协议解析）
        http_res_process* http_proc = new http_res_process(conn);

        // 创建异步数据处理器
        async_http_data_process* data_proc = new async_http_data_process(http_proc);

        // 关联
        http_proc->set_process(data_proc);

        // 设置连接
        conn->set_process(http_proc);
        conn->set_net_container(_container);

        // 添加到容器
        std::shared_ptr<base_net_obj> net_obj = conn;
        _container->push_real_net(net_obj);

        PDEBUG("[async_factory] connection enqueued to container thread=%u",
               _container->get_thread_index());
    }

    virtual void register_worker(uint32_t thread_index) override
    {
        _worker_indices.push_back(thread_index);
    }

    virtual void on_accept(base_net_thread* listen_th, int fd) override
    {
        if (!listen_th) return;

        uint32_t target_index = listen_th->get_thread_index();
        if (!_worker_indices.empty()) {
            unsigned long idx = _rr_hint++;
            target_index = _worker_indices[idx % _worker_indices.size()];
        }

        std::shared_ptr<content_msg> cm(new content_msg());
        cm->fd = fd;
        std::shared_ptr<normal_msg> ng = std::static_pointer_cast<normal_msg>(cm);

        ObjId id;
        id._id = OBJ_ID_THREAD;
        id._thread_index = target_index;
        base_net_thread::put_obj_msg(id, ng);
    }

private:
    common_obj_container* _container;
    std::vector<uint32_t> _worker_indices;
    std::atomic<unsigned long> _rr_hint;
};

// 全局服务器指针
server* g_server = nullptr;

void signal_handler(int sig)
{
    std::cout << "\n[Signal] Stopping async HTTP server..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char** argv)
{
    unsigned short port = 8090;
    if (argc > 1) {
        port = (unsigned short)atoi(argv[1]);
    }

    std::cout << "=== Async HTTP Server Demo ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "\nThis demo showcases async HTTP response handling:" << std::endl;
    std::cout << "  - /sync  -> Immediate response (synchronous)" << std::endl;
    std::cout << "  - /async -> 1-second delayed response (asynchronous)" << std::endl;
    std::cout << "\nKey implementation points:" << std::endl;
    std::cout << "  1. Call set_async_response_pending(true) in msg_recv_finish()" << std::endl;
    std::cout << "  2. Start timer or async operation" << std::endl;
    std::cout << "  3. In callback: complete_async_response()" << std::endl;
    std::cout << "     (or set_async_response_pending(false) + notify_send_ready())" << std::endl;
    std::cout << "\n-------------------------------------------------\n" << std::endl;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server srv;
    g_server = &srv;

    auto biz_factory = std::make_shared<async_http_factory>();
    auto listen_factory = std::make_shared<ListenFactory>("127.0.0.1", port, biz_factory);

    srv.set_business_factory(listen_factory);
    srv.start();

    std::cout << "✓ Async HTTP server started successfully!" << std::endl;
    std::cout << "\nTest commands:" << std::endl;
    std::cout << "  # Sync request (immediate):" << std::endl;
    std::cout << "  curl http://127.0.0.1:" << port << "/sync" << std::endl;
    std::cout << "\n  # Async request (1s delay):" << std::endl;
    std::cout << "  time curl http://127.0.0.1:" << port << "/async" << std::endl;
    std::cout << "\nPress Ctrl+C to stop\n" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}
