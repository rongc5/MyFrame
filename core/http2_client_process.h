#pragma once

#include "base_data_process.h"
#include <string>
#include <map>
#include <vector>

// Minimal HTTP/2 client data process for one-shot GET/POST.
// - Sends connection preface + SETTINGS
// - Sends a single request on stream 1 (HEADERS, optional DATA)
// - Receives SETTINGS/HEADERS/DATA/CONTINUATION and collects status/body
// - On END_STREAM, prints result and stops all threads
class http2_client_process : public base_data_process {
public:
    http2_client_process(std::shared_ptr<base_net_obj> c,
                         const std::string& method,
                         const std::string& scheme,
                         const std::string& host,
                         const std::string& path,
                         const std::map<std::string,std::string>& headers,
                         const std::string& body);
    virtual ~http2_client_process() {}

    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override;
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override;

private:
    void enqueue_preface_and_request();
    std::string build_headers_block() const; // HPACK (no Huffman), pseudo + user headers

    // parsing helpers
    bool parse_frames();
    bool handle_headers_payload(const unsigned char* p, uint32_t len, uint8_t flags);

private:
    std::string _method, _scheme, _host, _path, _body;
    std::map<std::string,std::string> _headers;
    bool _sent_all{false};
    bool _enqueued{false};
    std::vector<unsigned char> _in;
    int _status{0};
    std::string _resp_body;
    bool _response_done{false};
    bool _headers_end_stream{false};

    // Debug/trace knob
    bool _trace{false};

    // Flow-control: batch WINDOW_UPDATE to reduce control frames (configurable)
    uint32_t _conn_win_credits{0};
    uint32_t _strm1_win_credits{0};
    uint32_t _winupdate_threshold{32768}; // default 32KB; env MYFRAME_H2_WINUPDATE

    // Timers
    uint64_t _start_ms{0};
    bool _ping_scheduled{false};
    bool _timeout_scheduled{false};
    uint32_t _ping_interval_ms{15000};   // env MYFRAME_H2_PING_MS
    uint32_t _total_timeout_ms{30000};   // env MYFRAME_H2_TIMEOUT_MS
};
