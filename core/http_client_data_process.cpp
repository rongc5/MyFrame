#include "http_client_data_process.h"
#include "http_req_process.h"
#include "http_base_process.h"
#include <sstream>
#include <iostream>
#include "base_thread.h"
#include <cstring>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

http_client_data_process::http_client_data_process(http_req_process* p,
                                                   const std::string& method,
                                                   const std::string& host,
                                                   const std::string& path,
                                                   const std::map<std::string,std::string>& headers,
                                                   const std::string& body)
    : http_base_data_process((http_base_process*)p)
    , _method(method), _host(host), _path(path), _headers(headers), _body(body)
{
}

std::string* http_client_data_process::get_send_head() {
    if (_head_sent) return nullptr;
    std::stringstream ss;
    ss << _method << " " << (_path.empty()?"/":_path) << " HTTP/1.1\r\n";
    auto has_ci = [&](const char* key){
        for (auto& kv : _headers) {
            if (kv.first.size() == std::strlen(key) && strncasecmp(kv.first.c_str(), key, kv.first.size()) == 0) return true;
        }
        return false;
    };
    if (!has_ci("Host")) ss << "Host: " << _host << "\r\n";
    if (!has_ci("Connection")) ss << "Connection: close\r\n";
    if (!has_ci("User-Agent")) ss << "User-Agent: myframe-http-client\r\n";
    const char* no_ae = ::getenv("MYFRAME_NO_DEFAULT_AE");
    if (!has_ci("Accept-Encoding") && !(no_ae && (strcmp(no_ae, "1")==0 || strcasecmp(no_ae, "true")==0))) {
        ss << "Accept-Encoding: gzip\r\n";
    }
    for (auto& kv : _headers) ss << kv.first << ": " << kv.second << "\r\n";
    if (!_body.empty()) ss << "Content-Length: " << _body.size() << "\r\n";
    ss << "\r\n";
    _head_sent = true;
    return new std::string(ss.str()); // ownership transferred to transport layer
}

std::string* http_client_data_process::get_send_body(int& result) {
    if (_body.empty() || _body_sent) { result = 1; return nullptr; }
    _body_sent = true;
    result = 1; return new std::string(_body); // ownership transferred
}

size_t http_client_data_process::process_recv_body(const char* buf, size_t len, int& result) {
    if (buf && len) _resp_body.append(buf, len);
    result = 0; return len;
}

void http_client_data_process::msg_recv_finish() {
    // read status and headers from response head
    auto& hp = _base_process->get_res_head_para();
    _status = (int)hp._response_code;

    // Print response for debugging
    std::cout << "HTTP Response Status: " << _status << std::endl;
    std::cout << "Response Body Length: " << _resp_body.length() << std::endl;
#ifdef HAVE_ZLIB
    // Check content-encoding header or gzip magic, try to auto-decompress
    std::string* enc = hp.get_header("Content-Encoding");
    bool is_gzip = false;
    if (enc) {
        std::string v = *enc; for (auto& c : v) c = (char)tolower((unsigned char)c);
        if (v.find("gzip") != std::string::npos) is_gzip = true;
    }
    if (!is_gzip && _resp_body.size() >= 2 && (uint8_t)_resp_body[0] == 0x1f && (uint8_t)_resp_body[1] == 0x8b) {
        is_gzip = true;
    }
    if (is_gzip) {
        z_stream strm; memset(&strm, 0, sizeof(strm));
        int ret = inflateInit2(&strm, 16 + MAX_WBITS);
        if (ret == Z_OK) {
            strm.next_in = (Bytef*)_resp_body.data();
            strm.avail_in = (uInt)_resp_body.size();
            std::string dec; std::string out; out.resize(64 * 1024);
            int zret = Z_OK;
            while (zret == Z_OK) {
                out.resize(dec.size() + 64 * 1024);
                strm.next_out = (Bytef*)(&out[dec.size()]);
                strm.avail_out = 64 * 1024;
                zret = inflate(&strm, Z_NO_FLUSH);
                size_t have = 64 * 1024 - strm.avail_out;
                dec.append(out.data() + dec.size(), have);
            }
            inflateEnd(&strm);
            if (zret == Z_STREAM_END) {
                std::cout << "Decompressed (gzip) Length: " << dec.size() << std::endl;
                if (dec.size() < 1000) std::cout << "Response Body (gunzipped): " << dec << std::endl;
                else std::cout << "Response Body (gunzipped, first 500 chars): " << dec.substr(0,500) << "..." << std::endl;
            } else {
                std::cout << "[http] gzip decompress failed, zret=" << zret << std::endl;
                if (_resp_body.length() < 1000) std::cout << "Response Body (raw): " << _resp_body << std::endl;
                else std::cout << "Response Body (raw, first 500 chars): " << _resp_body.substr(0, 500) << "..." << std::endl;
            }
        } else {
            if (_resp_body.length() < 1000) std::cout << "Response Body (raw): " << _resp_body << std::endl;
            else std::cout << "Response Body (raw, first 500 chars): " << _resp_body.substr(0, 500) << "..." << std::endl;
        }
    } else {
        if (_resp_body.length() < 1000) std::cout << "Response Body: " << _resp_body << std::endl;
        else std::cout << "Response Body (first 500 chars): " << _resp_body.substr(0, 500) << "..." << std::endl;
    }
#else
    if (_resp_body.length() < 1000) {
        std::cout << "Response Body: " << _resp_body << std::endl;
    } else {
        std::cout << "Response Body (first 500 chars): " << _resp_body.substr(0, 500) << "..." << std::endl;
    }
#endif

    // Stop all network threads so event loop exits.
    // Using direct stop avoids timer routing edge-cases here.
    base_thread::stop_all_thread();
}

void http_client_data_process::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (t_msg && t_msg->_timer_type == STOP_THREAD_TIMER_TYPE) {
        base_thread::stop_all_thread();
        return;
    }
    http_base_data_process::handle_timeout(t_msg);
}
