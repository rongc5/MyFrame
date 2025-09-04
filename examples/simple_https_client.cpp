#include <iostream>
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 7777;
    std::string path = "/hello";
    if (argc > 1) port = atoi(argv[1]);

    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { std::cerr << "connect fail\n"; return 1; }

    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { std::cerr << "SSL_connect fail\n"; return 1; }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    SSL_write(ssl, req.c_str(), req.size());

    char buf[4096]; int n;
    std::string resp;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) resp.append(buf, n);
    std::cout << resp << std::endl;

    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return 0;
}

