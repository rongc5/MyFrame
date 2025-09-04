#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <cstring>

// 简化的WebSocket客户端（仅用于测试连接建立）
class SimpleWebSocketClient {
public:
    static bool test_ws_upgrade(const std::string& host, int port, const std::string& path = "/chat") {
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
        
        // 构造WebSocket升级请求
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";
        
        // 发送升级请求
        ssize_t sent = send(sock, request.c_str(), request.length(), 0);
        if (sent != (ssize_t)request.length()) {
            std::cerr << "发送升级请求失败" << std::endl;
            close(sock);
            return false;
        }
        
        // 接收升级响应
        char buf[1024];
        ssize_t received = recv(sock, buf, sizeof(buf)-1, 0);
        if (received <= 0) {
            std::cerr << "接收升级响应失败" << std::endl;
            close(sock);
            return false;
        }
        
        buf[received] = '\0';
        std::string response(buf);
        
        // 检查是否升级成功
        if (response.find("101 Switching Protocols") != std::string::npos &&
            response.find("Upgrade: websocket") != std::string::npos) {
            std::cout << "✅ WebSocket协议升级成功" << std::endl;
            close(sock);
            return true;
        } else {
            std::cerr << "❌ WebSocket升级失败" << std::endl;
            std::cerr << "响应: " << response << std::endl;
            close(sock);
            return false;
        }
    }
    
    static bool test_ws_connection(const std::string& host, int port) {
        // 简单的连接测试，检查协议检测是否工作
        std::cout << "测试WebSocket协议检测..." << std::endl;
        return test_ws_upgrade(host, port, "/websocket");
    }
};

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8080;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    std::cout << "=== WebSocket客户端测试 ===" << std::endl;
    std::cout << "目标: " << host << ":" << port << std::endl;
    
    // 测试WebSocket协议升级
    if (SimpleWebSocketClient::test_ws_connection(host, port)) {
        std::cout << "🎉 WebSocket测试通过！" << std::endl;
        std::cout << "协议检测和切换功能正常" << std::endl;
        return 0;
    } else {
        std::cout << "❌ WebSocket测试失败" << std::endl;
        std::cout << "请确保服务器支持WebSocket协议升级" << std::endl;
        return 1;
    }
}