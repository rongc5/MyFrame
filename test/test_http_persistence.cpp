#include "protocol_adapters/http_context_adapter.h"
#include <cassert>
#include <memory>
#include <string>

class PersistenceHandler : public myframe::IProtocolHandler {
public:
    std::string selection;
    bool complete_explicitly = false;
    void on_http_request(myframe::HttpContext& context) override {
        context.response().set_text("ok");
        if (!selection.empty()) context.response().set_header("cOnNeCtIoN", selection);
        if (complete_explicitly) context.complete_async_response();
    }
};

int main() {
    struct Case { const char* version; const char* requested; const char* selected; bool persistent; };
    const Case cases[] = {
        {"HTTP/1.1", "", "", true},
        {"HTTP/1.1", "keep-alive", "", true},
        {"HTTP/1.1", "Keep-Alive, ClOsE", "keep-alive", false},
        {"HTTP/1.1", "", "keep-alive, close", false},
        {"HTTP/1.0", "", "", false},
        {"HTTP/1.0", "keep-alive", "", true},
        {"HTTP/1.0", "close", "keep-alive", false},
    };
    for (bool explicit_completion : {false, true}) for (const auto& test : cases) {
        PersistenceHandler handler; handler.selection = test.selected;
        handler.complete_explicitly = explicit_completion;
        myframe::HttpContextAdapter process({}, &handler);
        std::string request = std::string("HEAD / ") + test.version + "\r\nHost: localhost\r\n";
        if (test.requested[0]) request += std::string("cOnNeCtIoN: ") + test.requested + "\r\n";
        request += "\r\n";
        assert(process.process_recv_buf(request.data(), request.size()) == request.size());
        std::unique_ptr<std::string> response(process.get_send_buf());
        assert(response);
        const std::string expected = test.persistent ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        assert(response->find(expected) != std::string::npos);
        assert(response->find("cOnNeCtIoN:") == std::string::npos);
        assert(response->find("Content-Length: 2\r\n") != std::string::npos);
        std::unique_ptr<std::string> body(process.get_send_buf());
        assert(!body || body->empty());
    }
}
