#include "connection_manager.h"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sstream>

namespace myframe {

// HttpProcessor 实现
size_t HttpProcessor::process(const char* data, size_t len) {
    if (!has_complete_request_) {
        // 简单检查是否有完整的HTTP请求
        std::string data_str(data, len);
        if (data_str.find("\r\n\r\n") != std::string::npos) {
            has_complete_request_ = true;
            
            // 生成HTTP响应
            auto response = std::unique_ptr<std::string>(new std::string());
            *response = "HTTP/1.1 200 OK\r\n";
            *response += "Content-Type: text/plain\r\n";
            *response += "X-Framework: MyFrame-Modern\r\n";
            *response += "Connection: close\r\n";
            *response += "\r\n";
            *response += "Hello from C++11 Modernized MyFrame!\n";
            
            pending_responses_.push_back(std::move(response));
            return len; // 处理了所有数据
        }
    }
    return 0; // 需要更多数据
}

std::unique_ptr<std::string> HttpProcessor::get_response() {
    if (pending_responses_.empty()) {
        return nullptr;
    }
    
    auto response = std::move(pending_responses_.front());
    pending_responses_.erase(pending_responses_.begin());
    return response;
}

void HttpProcessor::on_peer_close() {
    std::cout << "HTTP client disconnected" << std::endl;
}

// Connection 实现
Connection::Connection(int fd) : socket_fd_(fd), processor_(nullptr) {
    // 设置TCP_NODELAY
    int nodelay = 1;
    setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

Connection::~Connection() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
}

void Connection::set_processor(std::unique_ptr<DataProcessor> proc) {
    processor_ = std::move(proc);
}

ConnectionStatus Connection::recv_data(void* buf, size_t len, ssize_t& bytes_received) {
    bytes_received = recv(socket_fd_, buf, len, MSG_DONTWAIT);
    
    if (bytes_received == 0) {
        return ConnectionStatus::PEER_CLOSED;
    }
    else if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ConnectionStatus::WANT_READ;
        }
        return ConnectionStatus::FATAL_ERROR;
    }
    
    return ConnectionStatus::OK;
}

ConnectionStatus Connection::send_data(const void* buf, size_t len, ssize_t& bytes_sent) {
    bytes_sent = send(socket_fd_, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    
    if (bytes_sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ConnectionStatus::WANT_WRITE;
        }
        return ConnectionStatus::FATAL_ERROR;
    }
    
    return ConnectionStatus::OK;
}

ConnectionStatus Connection::receive_data() {
    if (!processor_) {
        return ConnectionStatus::FATAL_ERROR;
    }
    
    char buffer[8192];
    ssize_t bytes_received = 0;
    ConnectionStatus status = recv_data(buffer, sizeof(buffer), bytes_received);
    
    switch (status) {
        case ConnectionStatus::OK:
            if (bytes_received > 0) {
                recv_buffer_.append(buffer, bytes_received);
                
                // 让处理器处理数据
                size_t processed = processor_->process(recv_buffer_.c_str(), recv_buffer_.length());
                if (processed > 0) {
                    recv_buffer_.erase(0, processed);
                }
            }
            return ConnectionStatus::OK;
            
        case ConnectionStatus::WANT_READ:
        case ConnectionStatus::PEER_CLOSED:
        case ConnectionStatus::FATAL_ERROR:
            return status;
    }
    
    return ConnectionStatus::OK;
}

ConnectionStatus Connection::send_responses() {
    if (!processor_) {
        return ConnectionStatus::FATAL_ERROR;
    }
    
    auto response = processor_->get_response();
    if (!response) {
        return ConnectionStatus::OK; // 没有数据要发送
    }
    
    ssize_t bytes_sent = 0;
    ConnectionStatus status = send_data(response->c_str(), response->length(), bytes_sent);
    
    return status;
}

void Connection::close_connection() {
    if (processor_) {
        processor_->on_peer_close();
    }
    
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

// ConnectionFactory 实现
std::unique_ptr<Connection> ConnectionFactory::create_http_connection(int fd) {
    auto conn = std::unique_ptr<Connection>(new Connection(fd));
    auto processor = std::unique_ptr<DataProcessor>(new HttpProcessor());
    
    conn->set_processor(std::move(processor));
    return conn;
}

} // namespace myframe