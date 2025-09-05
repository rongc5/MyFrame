#pragma once

#include <string>

class WsSyncClient {
public:
    WsSyncClient();
    ~WsSyncClient();
    // url: ws:// or wss://
    bool connect(const std::string& url, const std::string& extra_headers = std::string());
    bool send_text(const std::string& msg);
    // blocking receive one text frame (simple, no fragmentation handling)
    bool recv_text(std::string& out);
    void close();
private:
    int _fd;
    void* _ssl_ctx;
    void* _ssl;
    std::string _host;
    std::string _port;
    std::string _path;
    bool _use_tls;
    bool handshake();
    bool readn(void* buf, size_t n);
    int  read_some(void* buf, size_t n);
    bool writen(const void* buf, size_t n);
};

