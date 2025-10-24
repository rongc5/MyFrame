// MyFrame Level 3 Demo - custom protocol via base_data_process
// Simple text protocol: client sends lines prefixed with "RAW " and terminated
// with '\n'. Server replies "OK <line>". Demonstrates direct use of
// base_data_process without Level 1/2 abstractions.

#include "server.h"
#include "unified_protocol_factory.h"
#include "base_data_process.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using namespace myframe;

namespace {

class RawEchoProcess : public base_data_process {
public:
    explicit RawEchoProcess(std::shared_ptr<base_net_obj> conn)
        : base_data_process(std::move(conn)) {}

    static bool can_handle(const char* data, size_t len) {
        return len >= 4 && std::memcmp(data, "RAW ", 4) == 0;
    }

    static std::unique_ptr<base_data_process> create(std::shared_ptr<base_net_obj> conn) {
        return std::unique_ptr<base_data_process>(new RawEchoProcess(conn));
    }

    size_t process_recv_buf(const char* buf, size_t len) override {
        _buffer.append(buf, len);
        size_t consumed = len;

        // Remove prefix if present (only once)
        if (!_handshake_done && _buffer.size() >= 4) {
            if (std::memcmp(_buffer.data(), "RAW ", 4) != 0) {
                // Invalid prefix -> close connection
                close_now();
                return consumed;
            }
            _buffer.erase(0, 4);
            _handshake_done = true;
            put_send_copy("WELCOME RAW MODE\n");
        }

        while (true) {
            auto pos = _buffer.find('\n');
            if (pos == std::string::npos) {
                break;
            }
            std::string line = _buffer.substr(0, pos);
            _buffer.erase(0, pos + 1);

            if (line == "quit" || line == "exit") {
                put_send_copy("BYE\n");
                request_close_after(50);
                continue;
            }

            put_send_copy("OK " + line + "\n");
        }

        return consumed;
    }

private:
    std::string _buffer;
    bool _handshake_done{false};
};

struct Options {
    unsigned short port = 7070;
};

Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            opt.port = static_cast<unsigned short>(std::atoi(argv[++i]));
        }
    }
    return opt;
}

server* g_srv = nullptr;

void stop_server(int) {
    std::cout << "\nStopping Level 3 demo..." << std::endl;
    if (g_srv) g_srv->stop();
    std::exit(0);
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt = parse_args(argc, argv);

    std::cout << "==============================" << std::endl;
    std::cout << " Level 3 Custom Protocol Demo" << std::endl;
    std::cout << "==============================" << std::endl;
    std::cout << "Port        : " << opt.port << std::endl;
    std::cout << "Protocol    : RAW text (custom base_data_process)" << std::endl;

    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        server srv(2);
        g_srv = &srv;

        auto factory = std::make_shared<UnifiedProtocolFactory>();
        factory->register_custom_protocol(
            "raw-echo",
            RawEchoProcess::can_handle,
        [](std::shared_ptr<base_net_obj> conn) {
                return RawEchoProcess::create(conn);
            },
            10);

        srv.bind("0.0.0.0", opt.port);
        srv.set_business_factory(factory);
        srv.start();

        std::cout << "\nTest commands:" << std::endl;
        std::cout << "  printf 'RAW hello\\n' | nc 127.0.0.1 " << opt.port << std::endl;
        std::cout << "  printf 'RAW foo\\nbar\\n' | nc 127.0.0.1 " << opt.port << std::endl;

        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
