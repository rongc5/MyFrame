#pragma once

#include "base_data_process.h"
#include "http2_frame.h"
#include <deque>
#include <vector>
#include "app_handler_v2.h"
#include <unordered_map>
#include <map>

class http2_process : public base_data_process {
public:
    enum {
        kLocalMaxFrameSize = 16384,
        kMaxConcurrentStreams = 100,
        kDefaultHeaderTableSize = 4096
    };

    explicit http2_process(std::shared_ptr<base_net_obj> c, myframe::IApplicationHandler* app=nullptr)
        : base_data_process(c)
        , _app(app)
        , _preface_ok(false)
        , _sent_settings(false)
        , _got_client_settings(false)
    {}

    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override { return base_data_process::get_send_buf(); }
    virtual void reset() override {
        _in.clear();
        _preface_ok = false;
        _sent_settings = false;
        _got_client_settings = false;
        _assembling = false;
        _assembling_sid = 0;
        _assembling_end_stream = false;
        _assembling_block.clear();
        _streams.clear();
        _last_stream_id = 0;
        _dec_dyn.clear();
        _dec_tbl_size = 0;
        _dec_tbl_max = kDefaultHeaderTableSize;
        _enc_dyn.clear();
        _enc_tbl_size = 0;
        _enc_tbl_max = kDefaultHeaderTableSize;
        _enc_size_pending = false;
    }
    virtual void handle_msg(std::shared_ptr<normal_msg>& msg) override;
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override;

    // Exposed for unit tests inspecting stream cleanup.
    size_t active_stream_count() const { return _streams.size(); }

private:
    void on_connected_once();
    bool parse_frames(size_t& consumed);
    bool handle_headers_block(uint32_t stream_id, const std::string& block, bool end_stream);
    void send_response(uint32_t stream_id, const myframe::HttpResponse& rsp,
                       bool suppress_body = false,
                       int32_t initial_send_window = -1);
    void on_data(uint32_t stream_id, const unsigned char* p, uint32_t len, bool end_stream);
    void finish_stream(uint32_t stream_id);
    void send_goaway_and_throw(h2::ErrorCode ec, const char* msg);

    std::string _out;
    std::vector<unsigned char> _in;
    myframe::IApplicationHandler* _app;
    bool _preface_ok;
    bool _sent_settings;
    bool _got_client_settings;

    // header assembly across CONTINUATION
    bool _assembling{false};
    uint32_t _assembling_sid{0};
    bool _assembling_end_stream{false};
    bool _assembling_self_dep{false};
    std::string _assembling_block;

    struct StreamState {
        std::string method;
        std::string path;
        std::string authority;
        std::map<std::string,std::string> headers;
        std::string body;
        bool has_content_length{false};
        uint64_t content_length{0};
        bool headers_received{false};
        bool end_stream_received{false};
        // Priority & flow control
        uint32_t dependency{0};
        uint8_t weight{16}; // 1-256 (stored as weight-1 on wire)
        int32_t send_window{65535};
        int32_t recv_window{65535};
        // Outbound response body (pending due to flow control)
        std::string out_body;
        size_t out_off{0};
        // Scheduler state (deficit round-robin)
        int32_t sched_deficit{0};
        uint32_t sched_quantum{16384}; // base quantum in bytes (scaled by weight)
    };
    std::unordered_map<uint32_t, StreamState> _streams;
    uint32_t _last_stream_id{0};

    // Connection-level flow control and settings
    int32_t _conn_send_window{65535};
    int32_t _conn_recv_window{65535};
    uint32_t _peer_initial_window_size{65535};
    uint32_t _peer_max_frame_size{16384};
    unsigned long _send_rr{0};

    uint32_t try_send_data(uint32_t stream_id);
    void pump_all_streams();
    void update_quantum(StreamState& st);

    // HPACK dynamic table entry (RFC 7541 4.1: size = 32 + name + value)
    struct DynHdr { std::string name; std::string value; size_t sz; };

    // Decoder dynamic table (per-connection, RFC 7541)
    std::deque<DynHdr> _dec_dyn; // newest at front (index 62 = static_table().size() + 1)
    size_t _dec_tbl_size{0};
    size_t _dec_tbl_max{kDefaultHeaderTableSize};
    size_t _dec_tbl_hard_max{kDefaultHeaderTableSize};
    void dec_dyn_evict();
    void dec_dyn_add(const std::string& name, const std::string& value);
    bool dec_lookup(uint32_t idx, std::string& name, std::string& value) const;

    // Encoder dynamic table (kept inactive; responses use literal-without-indexing)
    std::vector<DynHdr> _enc_dyn;
    size_t _enc_tbl_size{0};
    size_t _enc_tbl_max{kDefaultHeaderTableSize};
    bool _enc_size_pending{false};
    size_t _enc_pending_min{kDefaultHeaderTableSize};
    void enc_dyn_evict();
    void enc_dyn_add(const std::string& name, const std::string& value);
    uint32_t enc_dyn_find_pair(const std::string& name, const std::string& value) const;
};
