#pragma once

#include "base_data_process.h"
#include "http2_frame.h"
#include <vector>
#include "app_handler.h"
#include <unordered_map>
#include <map>

class http2_process : public base_data_process {
public:
    explicit http2_process(std::shared_ptr<base_net_obj> c, IAppHandler* app=nullptr)
        : base_data_process(c)
        , _app(app)
        , _preface_ok(false)
        , _sent_settings(false)
        , _got_client_settings(false)
    {}

    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override { return base_data_process::get_send_buf(); }
    virtual void reset() override { _in.clear(); _preface_ok=false; _sent_settings=false; _got_client_settings=false; }

private:
    void on_connected_once();
    bool parse_frames(size_t& consumed);
    bool handle_headers_block(uint32_t stream_id, const std::string& block, bool end_stream);
    void send_response(uint32_t stream_id, const HttpResponse& rsp);
    void on_data(uint32_t stream_id, const unsigned char* p, uint32_t len, bool end_stream);
    void finish_stream(uint32_t stream_id);

    std::string _out;
    std::vector<unsigned char> _in;
    IAppHandler* _app;
    bool _preface_ok;
    bool _sent_settings;
    bool _got_client_settings;

    // header assembly across CONTINUATION
    bool _assembling{false};
    uint32_t _assembling_sid{0};
    std::string _assembling_block;

    struct StreamState {
        std::string method;
        std::string path;
        std::string authority;
        std::map<std::string,std::string> headers;
        std::string body;
        // Priority & flow control
        uint32_t dependency{0};
        uint8_t weight{16}; // 1-256 (stored as weight-1 on wire)
        int32_t send_window{65535};
        int32_t recv_window{65535};
        // Outbound response body (pending due to flow control)
        std::string out_body;
        size_t out_off{0};
    };
    std::unordered_map<uint32_t, StreamState> _streams;

    // Connection-level flow control and settings
    int32_t _conn_send_window{65535};
    int32_t _conn_recv_window{65535};
    uint32_t _peer_initial_window_size{65535};
    uint32_t _peer_max_frame_size{16384};
    unsigned long _send_rr{0};

    void try_send_data(uint32_t stream_id);
    void pump_all_streams();
};
