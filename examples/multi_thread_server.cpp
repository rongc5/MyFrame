#include "../include/server.h"
#include "../include/multi_protocol_factory.h"
#include "../include/app_handler.h"
#include "../core/listen_factory.h"
#include <thread>
#include <clocale>
#include <locale>
#include <chrono>
#include <iostream>
#include <signal.h>
#include <atomic>
#include <vector>
#include <sstream>
#include <iomanip>

// 线程统计信息
struct ThreadStats {
    std::atomic<int> requests{0};
    std::atomic<int> connections{0};
    std::atomic<int> messages{0};
    std::thread::id thread_id;
    
    ThreadStats() : thread_id(std::this_thread::get_id()) {}
    
    // 移动构造函数
    ThreadStats(ThreadStats&& other) noexcept 
        : requests(other.requests.load()), connections(other.connections.load()), 
          messages(other.messages.load()), thread_id(other.thread_id) {}
    
    // 拷贝构造函数
    ThreadStats(const ThreadStats& other) 
        : requests(other.requests.load()), connections(other.connections.load()), 
          messages(other.messages.load()), thread_id(other.thread_id) {}
    
    // 禁用赋值操作符（原子变量不支持）
    ThreadStats& operator=(const ThreadStats&) = delete;
    ThreadStats& operator=(ThreadStats&&) = delete;
    
    std::string to_string() const {
        std::stringstream ss;
        ss << "Thread-" << thread_id << ": Req=" << requests.load() 
           << ", Conn=" << connections.load() << ", Msg=" << messages.load();
        return ss.str();
    }
};

// 全局统计 (每个工作线程一个)
std::vector<ThreadStats> g_thread_stats;
std::atomic<int> g_total_requests{0};
std::atomic<int> g_total_connections{0};
std::atomic<int> g_total_messages{0};
std::mutex g_stats_mutex;

// 获取当前线程的统计对象
ThreadStats* get_current_thread_stats() {
    thread_local ThreadStats* local_stats = nullptr;
    if (!local_stats) {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        auto thread_id = std::this_thread::get_id();
        for (auto& stats : g_thread_stats) {
            if (stats.thread_id == thread_id) {
                local_stats = &stats;
                break;
            }
        }
        if (!local_stats) {
            g_thread_stats.emplace_back();
            local_stats = &g_thread_stats.back();
        }
    }
    return local_stats;
}

// 简单的控制台本地化初始化，尽量启用 UTF-8
static void setup_console_locale() {
    // 仅设置 C 本地化，避免影响 iostream 数字分组等格式
    if (!std::setlocale(LC_ALL, "")) {
        // 回退到常见的 UTF-8 本地化名称
        std::setlocale(LC_ALL, "C.UTF-8");
        std::setlocale(LC_CTYPE, "C.UTF-8");
    }
}

// 高并发处理器：展示多线程负载均衡
class MultiThreadHandler : public IAppHandler {
private:
    std::atomic<int> _request_counter{0};
    
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        auto* stats = get_current_thread_stats();
        stats->requests++;
        g_total_requests++;
        
        int req_id = ++_request_counter;
        auto thread_id = std::this_thread::get_id();
        
        std::cout << "[HTTP-" << thread_id << "] 请求#" << req_id 
                  << " " << req.method << " " << req.url << std::endl;
        
        res.status = 200;
        res.set_header("Server", "MyFrame-MultiThread/1.0");
        res.set_header("X-Thread-ID", std::to_string(std::hash<std::thread::id>{}(thread_id)));
        res.set_header("X-Request-ID", std::to_string(req_id));
        res.set_header("Access-Control-Allow-Origin", "*");
        
        if (req.url == "/" || req.url == "/index.html") {
            res.set_header("Content-Type", "text/html; charset=utf-8");
            res.body = "<!DOCTYPE html><html><head><title>Multi-Thread Server</title></head><body>"
                      "<h1>MyFrame Multi-Thread Server</h1>"
                      "<p>Demonstrates 1 listener thread + multiple worker threads load balancing</p>"
                      "<h3>Test APIs:</h3>"
                      "<p><a href='/api/thread-info'>Thread Info</a> | "
                      "<a href='/api/thread-stats'>Thread Stats</a> | "
                      "<a href='/api/load-test'>Load Test</a></p>"
                      "<h3>Load Testing:</h3>"
                      "<p>Use curl or ab for load testing:</p>"
                      "<p><code>ab -n 1000 -c 10 http://localhost:9999/api/load-test</code></p>"
                      "<p><code>curl http://localhost:9999/api/thread-stats</code></p>"
                      "</body></html>";
        }
        else if (req.url == "/api/thread-info") {
            res.set_header("Content-Type", "application/json");
            std::stringstream ss;
            ss << "{"
               << "\"thread_count\":" << std::thread::hardware_concurrency() << ","
               << "\"current_thread\":\"" << thread_id << "\","
               << "\"request_id\":" << req_id
               << "}";
            res.body = ss.str();
        }
        else if (req.url == "/api/thread-stats") {
            res.set_header("Content-Type", "application/json");
            std::stringstream ss;
            ss << "{"
               << "\"total_requests\":" << g_total_requests.load() << ","
               << "\"total_connections\":" << g_total_connections.load() << ","
               << "\"total_messages\":" << g_total_messages.load() << ","
               << "\"threads\":[";
            
            std::lock_guard<std::mutex> lock(g_stats_mutex);
            for (size_t i = 0; i < g_thread_stats.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "\"" << g_thread_stats[i].to_string() << "\"";
            }
            ss << "]}";
            res.body = ss.str();
        }
        else if (req.url.find("/api/load-test") == 0) {
            res.set_header("Content-Type", "application/json");
            
            // 模拟一些工作负载
            std::this_thread::sleep_for(std::chrono::milliseconds(1 + (req_id % 10)));
            
            std::stringstream ss;
            ss << "{"
               << "\"request_id\":" << req_id << ","
               << "\"thread_id\":\"" << thread_id << "\","
               << "\"status\":\"ok\","
               << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count()
               << "}";
            res.body = ss.str();
        }
        else if (req.url == "/api/reset-stats") {
            if (req.method == "POST") {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                for (auto& stats : g_thread_stats) {
                    stats.requests = 0;
                    stats.connections = 0;
                    stats.messages = 0;
                }
                g_total_requests = 0;
                g_total_connections = 0;
                g_total_messages = 0;
                
                res.set_header("Content-Type", "application/json");
                res.body = "{\"status\":\"reset\"}";
            }
        }
        else {
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.body = "多线程服务器运行中\n"
                      "当前处理线程: " + std::to_string(std::hash<std::thread::id>{}(thread_id)) + "\n"
                      "请求编号: " + std::to_string(req_id) + "\n"
                      "访问 / 查看负载测试页面";
        }
    }
    
    void on_ws(const WsFrame& recv, WsFrame& send) override {
        auto* stats = get_current_thread_stats();
        stats->messages++;
        g_total_messages++;
        
        auto thread_id = std::this_thread::get_id();
        std::cout << "[WebSocket-" << thread_id << "] 消息: " << recv.payload << std::endl;
        
        if (recv.payload == "ping") {
            send = WsFrame::text("pong from thread " + std::to_string(std::hash<std::thread::id>{}(thread_id)));
        }
        else if (recv.payload == "thread-info") {
            std::stringstream ss;
            ss << "Thread: " << thread_id << ", Stats: " << stats->to_string();
            send = WsFrame::text(ss.str());
        }
        else {
            send = WsFrame::text("[Thread-" + std::to_string(std::hash<std::thread::id>{}(thread_id)) + "] Echo: " + recv.payload);
        }
    }
    
    void on_connect() override {
        auto* stats = get_current_thread_stats();
        stats->connections++;
        g_total_connections++;
        
        auto thread_id = std::this_thread::get_id();
        std::cout << "[连接-" << thread_id << "] 新连接 (总连接数: " 
                  << g_total_connections.load() << ")" << std::endl;
    }
    
    void on_disconnect() override {
        auto thread_id = std::this_thread::get_id();
        std::cout << "[断开-" << thread_id << "] 连接断开" << std::endl;
    }
};

server* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\n正在停止多线程服务器..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

void print_thread_stats() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        std::cout << "\n=== 线程统计 (每10秒) ===" << std::endl;
        std::cout << "总请求: " << g_total_requests.load() 
                  << ", 总连接: " << g_total_connections.load()
                  << ", 总消息: " << g_total_messages.load() << std::endl;
        
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        for (const auto& stats : g_thread_stats) {
            std::cout << "  " << stats.to_string() << std::endl;
        }
        std::cout << "=========================" << std::endl;
    }
}

int main(int argc, char** argv) {
    setup_console_locale();
    unsigned short port = 9999;
    int thread_count = 4; // 默认4个线程：1个监听 + 3个工作
    
    if (argc > 1) port = (unsigned short)atoi(argv[1]);
    if (argc > 2) thread_count = atoi(argv[2]);

    std::cout << "=== MyFrame 多线程服务器 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    std::cout << "线程数: " << thread_count << " (1个监听 + " << (thread_count-1) << "个工作)" << std::endl;
    std::cout << "CPU核心数: " << std::thread::hardware_concurrency() << std::endl;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 创建多线程服务器
    server srv(thread_count);
    g_server = &srv;
    
    // 预分配线程统计对象
    g_thread_stats.reserve(thread_count);
    
    // 创建处理器
    auto handler = std::make_shared<MultiThreadHandler>();
    auto biz = std::make_shared<MultiProtocolFactory>(handler.get());
    auto lsn = std::make_shared<ListenFactory>("127.0.0.1", port, biz);
    
    srv.set_business_factory(lsn);
    srv.start();

    // 启动统计线程
    std::thread stats_thread(print_thread_stats);
    stats_thread.detach();

    std::cout << "\n? 多线程服务器启动成功!" << std::endl;
    std::cout << "\n? 使用说明:" << std::endl;
    std::cout << "  浏览器访问: http://127.0.0.1:" << port << std::endl;
    std::cout << "  负载测试页面包含完整的功能演示" << std::endl;
    std::cout << "\n? 测试命令:" << std::endl;
    std::cout << "  curl http://127.0.0.1:" << port << "/api/thread-info" << std::endl;
    std::cout << "  ab -n 1000 -c 10 http://127.0.0.1:" << port << "/api/load-test" << std::endl;
    std::cout << "\n按 Ctrl+C 停止服务器\n" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    return 0;
}
