// Minimal IAppHandler adapter for MyFrame server examples
#pragma once

#ifdef USE_MYFRAME_SERVER

#include <memory>
#include <string>
#include "server.h"
#include "app_handler.h"

namespace StockQuery {

class MfAppHandler : public IAppHandler {
public:
    MfAppHandler() = default;
    ~MfAppHandler() override = default;

    void on_http(const HttpRequest& req, HttpResponse& res) override;
    void on_ws(const WsFrame& recv, WsFrame& send) override;
};

} // namespace StockQuery

#endif // USE_MYFRAME_SERVER

