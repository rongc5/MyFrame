#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>

class SimpleHttpClient {
public:
    static bool test_http_request(const std::string& host, int port, const std::string& path, 
                                 std::string& response, int expected_status = 200) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "创建socket失败" << std::endl;
            return false;
        }
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "连接失败: " << host << ":" << port << std::endl;
            close(sock);
            return false;
        }
        
        // 构造HTTP请求
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";
        
        // 发送请求
        ssize_t sent = send(sock, request.c_str(), request.length(), 0);
        if (sent != (ssize_t)request.length()) {
            std::cerr << "发送请求失败" << std::endl;
            close(sock);
            return false;
        }
        
        // 接收响应
        char buf[4096];
        response.clear();
        while (true) {
            ssize_t received = recv(sock, buf, sizeof(buf)-1, 0);
            if (received <= 0) break;
            buf[received] = '\0';
            response += buf;
        }
        
        close(sock);
        
        // 检查状态码
        if (response.find("HTTP/1.1 " + std::to_string(expected_status)) == std::string::npos) {
            std::cerr << "状态码不匹配，期望: " << expected_status << std::endl;
            return false;
        }
        
        return true;
    }
};

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8080;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    std::cout << "=== HTTP客户端测试 ===" << std::endl;
    std::cout << "目标: " << host << ":" << port << std::endl;
    
    std::string response;
    
    // 测试1
    std::cout << "\n测试 GET /hello:" << std::endl;
    if (SimpleHttpClient::test_http_request(host, port, "/hello", response)) {
        std::cout << "✅ 连接成功" << std::endl;
        std::cout << "响应长度: " << response.length() << " 字节" << std::endl;
    } else {
        std::cout << "❌ 连接失败" << std::endl;
        return 1;
    }
    
    // 测试2
    std::cout << "\n测试 GET /api/status:" << std::endl;
    if (SimpleHttpClient::test_http_request(host, port, "/api/status", response)) {
        std::cout << "✅ API测试通过" << std::endl;
    } else {
        std::cout << "❌ API测试失败" << std::endl;
    }
    
    std::cout << "\n🎉 HTTP客户端测试完成！" << std::endl;
    return 0;
}