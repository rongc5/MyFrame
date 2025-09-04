#pragma once

#include "../include/server.h"
#include <memory>
#include <string>

namespace myframe {

// 连接状态枚举 - 现代化错误处理
enum class ConnectionStatus {
    OK = 0,           // 操作成功
    WANT_READ,        // 需要更多数据才能继续
    WANT_WRITE,       // 需要等待可写事件
    PEER_CLOSED,      // 对端正常关闭连接
    FATAL_ERROR       // 致命错误，需要关闭连接
};

inline const char* to_string(ConnectionStatus status) {
    switch (status) {
        case ConnectionStatus::OK: return "OK";
        case ConnectionStatus::WANT_READ: return "WANT_READ";
        case ConnectionStatus::WANT_WRITE: return "WANT_WRITE";
        case ConnectionStatus::PEER_CLOSED: return "PEER_CLOSED";
        case ConnectionStatus::FATAL_ERROR: return "FATAL_ERROR";
        default: return "UNKNOWN";
    }
}

// 数据处理器接口
class DataProcessor {
public:
    virtual ~DataProcessor() = default;
    virtual size_t process(const char* data, size_t len) = 0;
    virtual std::unique_ptr<std::string> get_response() { return nullptr; }
    virtual void on_peer_close() {}
};

// HTTP处理器实现
class HttpProcessor : public DataProcessor {
public:
    virtual size_t process(const char* data, size_t len) override;
    virtual std::unique_ptr<std::string> get_response() override;
    virtual void on_peer_close() override;
    
private:
    std::vector<std::unique_ptr<std::string>> pending_responses_;
    bool has_complete_request_ = false;
};

// 现代化连接管理器 - 使用组合模式
class Connection {
public:
    explicit Connection(int fd);
    ~Connection();
    
    // 现代化的IO方法
    ConnectionStatus receive_data();
    ConnectionStatus send_responses();
    void close_connection();
    
    // 设置处理器 - 智能指针转移所有权
    void set_processor(std::unique_ptr<DataProcessor> proc);
    
    int get_fd() const { return socket_fd_; }
    
private:
    ConnectionStatus recv_data(void* buf, size_t len, ssize_t& bytes_received);
    ConnectionStatus send_data(const void* buf, size_t len, ssize_t& bytes_sent);
    
    int socket_fd_;
    std::string recv_buffer_;
    std::unique_ptr<DataProcessor> processor_;
};

// 连接工厂
class ConnectionFactory {
public:
    static std::unique_ptr<Connection> create_http_connection(int fd);
};

} // namespace myframe