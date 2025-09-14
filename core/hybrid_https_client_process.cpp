#include "hybrid_https_client_process.h"
#include "http2_client_process.h"
#include "client_ssl_codec.h"
#include "http2_frame.h"
#include "base_thread.h"
#include "out_connect.h"
#include <algorithm>
#include <cctype>
#include <iostream>

using namespace h2;

hybrid_https_client_process::hybrid_https_client_process(std::shared_ptr<base_net_obj> c,
                                                         const std::string& method,
                                                         const std::string& host,
                                                         const std::string& path,
                                                         const std::map<std::string,std::string>& headers,
                                                         const std::string& body)
    : base_data_process(c), _method(method), _host(host), _path(path), _headers(headers), _body(body) {}

bool hybrid_https_client_process::maybe_init() {
    if (_mode != UNKNOWN) return true;
    auto sp = _p_connect.lock(); if (!sp) return false;
    // out_connect<hybrid_https_client_process>
    auto conn = std::dynamic_pointer_cast< out_connect<hybrid_https_client_process> >(sp);
    if (!conn) return false;
    ICodec* codec = conn->get_codec();
    auto csc = dynamic_cast<ClientSslCodec*>(codec);
    if (!csc || !csc->is_handshake_done()) {
        return false;
    }
    std::string alpn = csc->selected_alpn();
    std::string alpn_l; alpn_l.resize(alpn.size());
    std::transform(alpn.begin(), alpn.end(), alpn_l.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    std::cout << "[https] negotiated ALPN='" << (alpn.empty()?std::string("(none)"):alpn) << "'" << std::endl;
    if (alpn_l == "h2") {
        // Switch to HTTP/2 client process
        _mode = H2;
        _inner.reset(new http2_client_process(sp, _method, std::string("https"), _host, _path, _headers, _body));
        std::cout << "[https] switch mode: H2" << std::endl;
        return true;
    } else {
        _mode = HTTP1;
        std::cout << "[https] switch mode: HTTP/1.1" << std::endl;
        return true;
    }
}

std::string* hybrid_https_client_process::get_send_buf() {
    if (_mode == UNKNOWN) {
        if (!maybe_init()) return nullptr; // wait handshake
    }
    if (_mode == H2) {
        return _inner ? _inner->get_send_buf() : nullptr;
    }
    // HTTP/1.1
    if (!_sent_http1) {
        enqueue_http1_request();
        _sent_http1 = true;
    }
    return base_data_process::get_send_buf();
}

static void to_lower(std::string& s){ for (auto& c : s) c = (char)std::tolower((unsigned char)c); }

void hybrid_https_client_process::enqueue_http1_request() {
    std::string head;
    head.reserve(512);
    const std::string& m = _method.empty()? std::string("GET"): _method;
    head += m; head += " "; head += (_path.empty()? std::string("/") : _path); head += " HTTP/1.1\r\n";
    head += "Host: "; head += _host; head += "\r\n";
    bool has_ua=false, has_ct=false, has_cl=false, has_ae=false;
    for (auto kv : _headers) {
        std::string name = kv.first; to_lower(name);
        if (name == "user-agent") has_ua = true;
        else if (name == "content-type") has_ct = true;
        else if (name == "content-length") has_cl = true;
        else if (name == "accept-encoding") has_ae = true;
        head += kv.first; head += ": "; head += kv.second; head += "\r\n";
    }
    if (!has_ua) head += "User-Agent: myframe-auto-https\r\n";
    const char* no_ae = ::getenv("MYFRAME_NO_DEFAULT_AE");
    if (!has_ae && !(no_ae && (strcmp(no_ae, "1")==0 || strcasecmp(no_ae, "true")==0))) {
        head += "Accept-Encoding: gzip\r\n";
    }
    if (!_body.empty() && !has_ct) head += "Content-Type: application/octet-stream\r\n";
    if (!_body.empty() && !has_cl) { head += "Content-Length: "; head += std::to_string(_body.size()); head += "\r\n"; }
    head += "Connection: close\r\n\r\n";
    put_send_move(std::move(head));
    if (!_body.empty()) put_send_copy(_body);
}

size_t hybrid_https_client_process::process_http1_recv(const char* buf, size_t len) {
    _h1_buf.append(buf, len);
    if (!_h1_headers_done) {
        auto pos = _h1_buf.find("\r\n\r\n");
        if (pos == std::string::npos) return len; // wait more
        // parse status line and headers
        std::string head = _h1_buf.substr(0, pos+2); // include last CRLF of header lines
        // status line
        auto eol = head.find("\r\n");
        if (eol != std::string::npos) {
            std::string status = head.substr(0, eol);
            auto sp1 = status.find(' ');
            auto sp2 = status.find(' ', sp1==std::string::npos?0:sp1+1);
            if (sp1!=std::string::npos && sp2!=std::string::npos) _h1_status = atoi(status.substr(sp1+1, sp2-sp1-1).c_str());
        }
        // headers
        size_t off = eol==std::string::npos? 0 : eol+2;
        while (off < head.size()) {
            size_t nl = head.find("\r\n", off);
            if (nl == std::string::npos || nl == off) break;
            std::string line = head.substr(off, nl-off);
            off = nl + 2;
            size_t col = line.find(':');
            if (col != std::string::npos) {
                std::string name = line.substr(0, col); to_lower(name);
                std::string value = line.substr(col+1); if (!value.empty() && value[0]==' ') value.erase(0,1);
                if (name == "content-length") { _h1_content_length = (size_t)strtoull(value.c_str(), 0, 10); }
                else if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) { _h1_chunked = true; }
            }
        }
        _h1_headers_done = true;
        _h1_buf.erase(0, pos+4);
    }
    // body
    if (_h1_chunked) {
        // Minimal chunked decode
        for(;;){
            // need at least a line
            size_t nl = _h1_buf.find("\r\n");
            if (nl == std::string::npos) break;
            std::string sz = _h1_buf.substr(0,nl);
            size_t chunk = (size_t)strtoul(sz.c_str(), 0, 16);
            if (_h1_buf.size() < nl+2+chunk+2) break; // wait
            if (chunk == 0) {
            // done (do not stop all threads; owner decides lifecycle)
            break;
            }
            _h1_body.append(_h1_buf.data()+nl+2, chunk);
            _h1_buf.erase(0, nl+2+chunk+2);
        }
    } else if (_h1_content_length > 0) {
        size_t need = _h1_content_length - _h1_body.size();
        size_t take = std::min(need, _h1_buf.size());
        _h1_body.append(_h1_buf.data(), take);
        _h1_buf.erase(0, take);
        if (_h1_body.size() >= _h1_content_length) {
            // complete; owner can decide to stop threads or keep running
        }
    } else {
        // no length: read until close; nothing to do here
    }
    return len;
}

size_t hybrid_https_client_process::process_recv_buf(const char* buf, size_t len) {
    if (_mode == UNKNOWN) {
        if (!maybe_init()) return 0; // wait handshake
    }
    if (_mode == H2) {
        return _inner ? _inner->process_recv_buf(buf, len) : 0;
    }
    return process_http1_recv(buf, len);
}

void hybrid_https_client_process::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (_inner) _inner->handle_timeout(t_msg);
}
