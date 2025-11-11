// MyFrame Level 3 Multi-Protocol Demo
// -----------------------------------
// This example demonstrates how to build a multi-protocol server using the
// Level 3 base_data_process API. We register three custom protocols on top of
// UnifiedProtocolFactory:
//   * HTTP      (custom http_res_process + http_base_data_process implementation)
//   * WebSocket (custom web_socket_res_process + web_socket_data_process)
//   * RAW text  (simple line-based echo similar to level3_custom_echo.cpp)
//
// Each protocol is detected by inspecting the first bytes on the connection
// and instantiating the corresponding base_data_process subclass.

#include "server.h"
#include "unified_protocol_factory.h"
#include "http_res_process.h"
#include "http_base_data_process.h"
#include "web_socket_res_process.h"
#include "web_socket_data_process.h"
#include "string_pool.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

using namespace myframe;

namespace {

// Utility: trim helpers for HTTP request parsing
inline void ltrim(std::string& s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
}

inline void rtrim(std::string& s) {
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
}

inline std::string to_lower_copy(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return input;
}

// -----------------------------------------------------------------------------
// Level 3 HTTP implementation
// -----------------------------------------------------------------------------

class Level3HttpDataProcess : public http_base_data_process {
public:
    explicit Level3HttpDataProcess(http_base_process* process)
        : http_base_data_process(process) {}

    std::string* get_send_head() override {
        if (!_head_ready || !_head_buffer) {
            return nullptr;
        }
        _head_ready = false;
        return _head_buffer.release();
    }

    std::string* get_send_body(int& result) override {
        if (!_body_ready || !_body_buffer) {
            result = 1;
            return nullptr;
        }
        _body_ready = false;
        result = 1;
        return _body_buffer.release();
    }

    void header_recv_finish() override {
        auto& req = _base_process->get_req_head_para();
        std::cout << "[L3 HTTP] header complete: " << req._method << " " << req._url_path << std::endl;
    }

    size_t process_recv_body(const char* buf, size_t len, int& result) override {
        if (buf && len) {
            _body.append(buf, len);
        }
        result = 0;
        return len;
    }

    void msg_recv_finish() override {
        auto& req = _base_process->get_req_head_para();
        std::string method = req._method.empty() ? "GET" : req._method;
        std::string url = req._url_path.empty() ? "/" : req._url_path;
        std::cout << "[L3 HTTP] request: " << method << " " << url << std::endl;

        if (url == "/api/async") {
            begin_async_response(url);
            return;
        }

        prepare_response(url, false);
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg) return;
        if (t_msg->_timer_type == HTTP_ASYNC_TIMER && _async_pending) {
            std::cout << "[L3 HTTP] async timer fired, completing response" << std::endl;
            prepare_response(_pending_url.empty() ? "/" : _pending_url, true);
            _async_pending = false;
            _pending_url.clear();
            complete_async_response();
        }
    }

private:
    static const int HTTP_ASYNC_TIMER = 0x7001;

    void prepare_response(const std::string& url, bool is_async) {
        int status = 200;
        std::string reason = "OK";
        std::string body;
        std::string content_type = "text/html; charset=utf-8";

        if (url == "/") {
            std::stringstream html;
            html << "<!DOCTYPE html><html><head><title>Level 3 Multi-Protocol</title></head><body>";
            html << "<h1>Level 3 Multi-Protocol Demo</h1>";
            html << "<p>This demo runs entirely on <strong>base_data_process</strong> implementations.</p>";
            html << "<ul>";
            html << "<li><a href='/api/hello'>/api/hello</a> (JSON)</li>";
            html << "<li><a href='/api/time'>/api/time</a> (server time)</li>";
            html << "<li><a href='/api/async'>/api/async</a> (1s delayed response)</li>";
            html << "</ul>";
            html << "<p>Try the other protocols:</p>";
            html << "<pre>";
            html << "  websocat ws://127.0.0.1:<PORT>/ws\\n";
            html << "  printf 'RAW hello\\\\n' | nc 127.0.0.1 <PORT>\\n";
            html << "</pre>";
            html << "</body></html>";
            body = html.str();
        } else if (url == "/api/hello") {
            content_type = "application/json";
            body = R"({"message":"hello from level3 http"})";
        } else if (url == "/api/time") {
            content_type = "application/json";
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_MSC_VER)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::stringstream json;
            json << "{\"iso_time\":\"" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
                 << "\",\"epoch\":" << static_cast<long long>(tt) << "}";
            body = json.str();
        } else if (url == "/api/async") {
            content_type = "application/json";
            body = R"({"mode":"async","delay_ms":1000})";
        } else {
            status = 404;
            reason = "Not Found";
            std::stringstream html;
            html << "<!DOCTYPE html><html><body>";
            html << "<h1>404 Not Found</h1>";
            html << "<p>No handler for " << url << "</p>";
            html << "</body></html>";
            body = html.str();
        }

        if (is_async) {
            body += "\n";
        }

        _head_buffer.reset(new std::string);
        *_head_buffer += "HTTP/1.1 ";
        *_head_buffer += std::to_string(status);
        *_head_buffer += " ";
        *_head_buffer += reason;
        *_head_buffer += "\r\n";
        *_head_buffer += "Content-Type: ";
        *_head_buffer += content_type;
        *_head_buffer += "\r\n";
        *_head_buffer += "Content-Length: ";
        *_head_buffer += std::to_string(body.size());
        *_head_buffer += "\r\n";
        *_head_buffer += "Connection: close\r\n";
        *_head_buffer += "Server: MyFrame-Level3/1.0\r\n\r\n";
        _head_ready = true;

        _body_buffer.reset(new std::string(std::move(body)));
        _body_ready = true;
        _body.clear();
    }

    void begin_async_response(const std::string& url) {
        if (_async_pending) {
            return;
        }
        std::cout << "[L3 HTTP] scheduling async response..." << std::endl;
        set_async_response_pending(true);
        _async_pending = true;
        _pending_url = url;

        auto timer = std::make_shared<timer_msg>();
        timer->_timer_type = HTTP_ASYNC_TIMER;
        timer->_time_length = 1000;
        if (auto conn = get_base_net()) {
            timer->_obj_id = conn->get_id()._id;
        }
        add_timer(timer);
    }

    std::string _body;
    std::unique_ptr<std::string> _head_buffer;
    std::unique_ptr<std::string> _body_buffer;
    bool _head_ready{false};
    bool _body_ready{false};
    bool _async_pending{false};
    std::string _pending_url;
};

class Level3HttpProcess : public http_res_process {
public:
    explicit Level3HttpProcess(std::shared_ptr<base_net_obj> conn)
        : http_res_process(std::move(conn)) {
        set_process(new Level3HttpDataProcess(this));
    }

    static bool can_handle(const char* data, size_t len) {
        if (!data || len < 4) {
            return false;
        }
        auto starts_with = [&](const char* prefix) {
            size_t prefix_len = std::strlen(prefix);
            return len >= prefix_len && std::strncmp(data, prefix, prefix_len) == 0;
        };

        bool method_ok = starts_with("GET ") || starts_with("POST ") ||
                         starts_with("HEAD ") || starts_with("PUT ") ||
                         starts_with("DELETE ") || starts_with("OPTIONS ") ||
                         starts_with("PATCH ");
        if (!method_ok) {
            return false;
        }

        std::string header(data, len);
        auto first_line_end = header.find("\r\n");
        if (first_line_end == std::string::npos) {
            return false;
        }

        auto header_end = header.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return false; // need full header to confirm protocol
        }

        std::string headers = to_lower_copy(header.substr(0, header_end));
        if (headers.find("upgrade: websocket") != std::string::npos) {
            return false; // let WebSocket detector handle it
        }
        return true;
    }

    static std::unique_ptr<base_data_process> create(std::shared_ptr<base_net_obj> conn) {
        return std::unique_ptr<base_data_process>(new Level3HttpProcess(std::move(conn)));
    }
};

// -----------------------------------------------------------------------------
// Level 3 WebSocket implementation
// -----------------------------------------------------------------------------

class Level3WsDataProcess : public web_socket_data_process {
public:
    explicit Level3WsDataProcess(web_socket_process* process)
        : web_socket_data_process(process) {}

    void on_handshake_ok() override {
        std::cout << "[L3 WS] handshake complete" << std::endl;
        ws_msg_type welcome;
        welcome.init();
        welcome._p_msg = myframe::string_acquire();
        welcome._p_msg->assign("Welcome to Level3 WebSocket echo server!");
        welcome._con_type = 0x1; // TEXT
        put_send_msg(welcome);
    }

    void msg_recv_finish() override {
        auto& hdr = _process->get_recent_recv_frame_header();
        std::cout << "[L3 WS] frame opcode=0x" << std::hex << (int)hdr._op_code
                  << std::dec << " payload='" << _recent_msg << "'" << std::endl;

        if (hdr._op_code == 0x1) { // text
            ws_msg_type reply;
            reply.init();
            reply._p_msg = myframe::string_acquire();
            reply._p_msg->assign("Echo: " + _recent_msg);
            reply._con_type = 0x1;
            put_send_msg(reply);
        } else if (hdr._op_code == 0x2) { // binary -> respond with text summary
            ws_msg_type reply;
            reply.init();
            reply._p_msg = myframe::string_acquire();
            reply._p_msg->assign("Binary payload size=" + std::to_string(_recent_msg.size()));
            reply._con_type = 0x1;
            put_send_msg(reply);
        } else if (hdr._op_code == 0x9) { // ping -> answer pong with same payload
            ws_msg_type pong;
            pong.init();
            pong._p_msg = myframe::string_acquire();
            pong._p_msg->assign(_recent_msg);
            pong._con_type = 0xA; // PONG
            put_send_msg(pong);
        } else if (hdr._op_code == 0x8) { // close
            std::cout << "[L3 WS] close frame received" << std::endl;
        }
        _recent_msg.clear();
    }
};

class Level3WsProcess : public web_socket_res_process {
public:
    explicit Level3WsProcess(std::shared_ptr<base_net_obj> conn)
        : web_socket_res_process(std::move(conn)) {
        set_process(new Level3WsDataProcess(this));
    }

    static bool can_handle(const char* data, size_t len) {
        if (!data || len < 4) {
            return false;
        }
        auto starts_with = [&](const char* prefix) {
            size_t prefix_len = std::strlen(prefix);
            return len >= prefix_len && std::strncmp(data, prefix, prefix_len) == 0;
        };
        if (!starts_with("GET ")) {
            return false;
        }

        std::string header(data, len);
        auto header_end = header.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return false;
        }

        std::string headers = to_lower_copy(header.substr(0, header_end));
        return headers.find("upgrade: websocket") != std::string::npos;
    }

    static std::unique_ptr<base_data_process> create(std::shared_ptr<base_net_obj> conn) {
        return std::unique_ptr<base_data_process>(new Level3WsProcess(std::move(conn)));
    }
};

// -----------------------------------------------------------------------------
// Level 3 RAW protocol (line-based echo)
// -----------------------------------------------------------------------------

class Level3RawProcess : public base_data_process {
public:
    explicit Level3RawProcess(std::shared_ptr<base_net_obj> conn)
        : base_data_process(std::move(conn)) {}

    static bool can_handle(const char* data, size_t len) {
        return data && len >= 4 && std::memcmp(data, "RAW ", 4) == 0;
    }

    static std::unique_ptr<base_data_process> create(std::shared_ptr<base_net_obj> conn) {
        return std::unique_ptr<base_data_process>(new Level3RawProcess(std::move(conn)));
    }

    size_t process_recv_buf(const char* buf, size_t len) override {
        _buffer.append(buf, len);
        size_t consumed = len;

        if (!_handshake_done && _buffer.size() >= 4) {
            if (std::memcmp(_buffer.data(), "RAW ", 4) != 0) {
                std::cout << "[L3 RAW] invalid handshake, closing" << std::endl;
                close_now();
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

// -----------------------------------------------------------------------------
// CLI options and helpers
// -----------------------------------------------------------------------------

struct Options {
    unsigned short port = 8181;
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
    std::cout << "\nStopping Level 3 multi-protocol demo..." << std::endl;
    if (g_srv) {
        g_srv->stop();
    }
    std::exit(0);
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt = parse_args(argc, argv);

    std::cout << "==============================\n";
    std::cout << " Level 3 Multi-Protocol Demo\n";
    std::cout << "==============================\n";
    std::cout << "Port        : " << opt.port << "\n";
    std::cout << "Protocols   : HTTP + WebSocket + RAW (custom)\n";
    std::cout << "Detection   : UnifiedProtocolFactory + Level 3 processes\n";

    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        server srv(4); // 1 listen + 3 workers
        g_srv = &srv;

        auto factory = std::make_shared<UnifiedProtocolFactory>();
        factory->register_custom_protocol(
                    "ws-level3",
                    Level3WsProcess::can_handle,
                    Level3WsProcess::create,
                    10)
                .register_custom_protocol(
                    "http-level3",
                    Level3HttpProcess::can_handle,
                    Level3HttpProcess::create,
                    20)
                .register_custom_protocol(
                    "raw-level3",
                    Level3RawProcess::can_handle,
                    Level3RawProcess::create,
                    30);

        srv.bind("0.0.0.0", opt.port);
        srv.set_business_factory(factory);
        srv.start();

        std::cout << "\nTest commands:\n";
        std::cout << "  curl http://127.0.0.1:" << opt.port << "/api/hello\n";
        std::cout << "  websocat ws://127.0.0.1:" << opt.port << "/ws\n";
        std::cout << "  printf 'RAW greetings\\n' | nc 127.0.0.1 " << opt.port << "\n";
        std::cout << "\nPress Ctrl+C to stop.\n";

        srv.join();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
