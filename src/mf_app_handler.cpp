#ifdef USE_MYFRAME_SERVER

#include "mf_app_handler.h"
#include <fstream>
#include <sstream>

namespace {
std::string read_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::string();
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}
}

namespace StockQuery {

void MfAppHandler::on_http(const HttpRequest& req, HttpResponse& res) {
    if (req.url == "/api/status") {
        res.status = 200;
        res.set_header("Content-Type", "application/json");
        res.body = "{\"status\":\"running\",\"service\":\"stock_query\"}";
        return;
    }
    if (req.url.rfind("/static/", 0) == 0) {
        std::string body = read_file("static/" + req.url.substr(std::string("/static/").size()));
        if (body.empty()) { res.status = 404; res.body = "Not Found"; return; }
        res.status = 200;
        res.set_header("Content-Type", "application/octet-stream");
        res.body = std::move(body);
        return;
    }
    // default page
    std::string body = read_file("templates/index.html");
    if (body.empty()) body = "<html><body><h1>Stock Query</h1><p>/api/status</p></body></html>";
    res.status = 200; res.set_header("Content-Type", "text/html; charset=utf-8");
    res.body = std::move(body);
}

void MfAppHandler::on_ws(const WsFrame& recv, WsFrame& send) {
    if (recv.payload == "ping") send = WsFrame::text("pong");
    else send = WsFrame::text("echo:" + recv.payload);
}

} // namespace StockQuery

#endif // USE_MYFRAME_SERVER

