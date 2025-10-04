// 压力测试：验证双队列模型在高并发下的表现
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

std::atomic<int> success_count{0};
std::atomic<int> error_count{0};

void send_http_request(const char* host, int port, int request_id) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        error_count++;
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        error_count++;
        close(sock);
        return;
    }

    // 发送HTTP请求
    std::string request = "GET /hello HTTP/1.1\r\nHost: " + std::string(host) +
                         "\r\nConnection: close\r\n\r\n";

    if (send(sock, request.c_str(), request.size(), 0) < 0) {
        error_count++;
        close(sock);
        return;
    }

    // 接收响应
    char buffer[4096];
    int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (bytes > 0) {
        buffer[bytes] = '\0';
        if (strstr(buffer, "200 OK") || strstr(buffer, "MULTI-PROTOCOL")) {
            success_count++;
        } else {
            error_count++;
        }
    } else {
        error_count++;
    }

    close(sock);
}

int main(int argc, char* argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 17888;
    int num_threads = argc > 2 ? atoi(argv[2]) : 10;
    int requests_per_thread = argc > 3 ? atoi(argv[3]) : 20;

    std::cout << "=== 双队列模型压力测试 ===" << std::endl;
    std::cout << "端口: " << port << std::endl;
    std::cout << "并发线程数: " << num_threads << std::endl;
    std::cout << "每线程请求数: " << requests_per_thread << std::endl;
    std::cout << "总请求数: " << num_threads * requests_per_thread << std::endl;
    std::cout << "\n开始测试..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int r = 0; r < requests_per_thread; ++r) {
                send_http_request("127.0.0.1", port, t * requests_per_thread + r);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    int total = num_threads * requests_per_thread;

    std::cout << "\n=== 测试结果 ===" << std::endl;
    std::cout << "✓ 成功: " << success_count.load() << std::endl;
    std::cout << "✗ 失败: " << error_count.load() << std::endl;
    std::cout << "总计: " << total << std::endl;
    std::cout << "成功率: " << (success_count.load() * 100.0 / total) << "%" << std::endl;
    std::cout << "总耗时: " << duration.count() << "ms" << std::endl;
    std::cout << "吞吐量: " << (total * 1000.0 / duration.count()) << " req/s" << std::endl;

    bool pass = (success_count.load() == total) && (error_count.load() == 0);
    std::cout << "\n" << (pass ? "✓ 测试通过" : "✗ 测试失败") << std::endl;

    return pass ? 0 : 1;
}
