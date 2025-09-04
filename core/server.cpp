#include "server.h"
#include <cstdio>

server::server(int thread_num)
    : _threads(thread_num > 0 ? thread_num : 1), _port(0), _running(false) {}

server::~server() {
    stop();
    join();
}

void server::bind(const std::string& ip, unsigned short port) {
    _ip = ip; _port = port;
    std::printf("[server] bind %s:%u\n", ip.c_str(), (unsigned)port);
}

void server::set_business_factory(std::shared_ptr<IConnectionFactory> factory) {
    _factory = factory;
    std::printf("[server] business factory set\n");
}

void server::start() {
    _running = true;
    std::printf("[server] start threads=%d on %s:%u\n", _threads, _ip.c_str(), (unsigned)_port);
}

void server::stop() {
    if (_running) {
        std::printf("[server] stop\n");
        _running = false;
    }
}

void server::join() {
    std::printf("[server] join\n");
}

size_t server::size() const { return (size_t)_threads; }

