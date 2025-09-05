#include "http2_process.h"
#include "common_exception.h"
#include "log_helper.h"
#include <cstring>
#include "hpack.h"
#include "http_base_process.h" // for HttpRequest/HttpResponse structures live in app_handler.h

using namespace h2;

void http2_process::on_connected_once() {
    if (_sent_settings) return;
    // Send server SETTINGS (ENABLE_PUSH=0)
    std::string settings = make_settings_frame(false);
    put_send_buf(new std::string(std::move(settings)));
    _sent_settings = true;
}

// HTTP/2 flag bits
static const uint8_t FLAG_END_STREAM = 0x1;
static const uint8_t FLAG_END_HEADERS = 0x4;
static const uint8_t FLAG_PADDED = 0x8;
static const uint8_t FLAG_PRIORITY = 0x20;

bool http2_process::parse_frames(size_t& consumed) {
    consumed = 0;
    const size_t n = _in.size();
    size_t off = 0;
    while (n - off >= 9) {
        const unsigned char* p = &_in[off];
        uint32_t len = read24(p);
        uint8_t  type = p[3];
        uint8_t  flags = p[4];
        uint32_t sid = read32(p + 5) & 0x7fffffffu;
        if (n - off < 9 + len) break; // incomplete frame

        const unsigned char* payload = p + 9;
        // Minimal handling for SETTINGS/HEADERS/PING/CONTINUATION
        if (type == SETTINGS) {
            if (flags & FLAGS_ACK) {
                // ignore
            } else {
                // validate pairs (6 bytes each) but ignore content
                if (len % 6 != 0) {
                    // protocol error → GOAWAY
                    std::string go = make_goaway(0, FRAME_SIZE_ERROR, "bad settings len");
                    put_send_buf(new std::string(std::move(go)));
                    throw CMyCommonException("http2: invalid SETTINGS length");
                }
                // parse settings
                for (uint32_t offp = 0; offp + 6 <= len; offp += 6) {
                    uint16_t id = ((uint16_t)payload[offp] << 8) | (uint16_t)payload[offp+1];
                    uint32_t val = read32(payload + offp + 2);
                    if (id == SETTINGS_INITIAL_WINDOW_SIZE) {
                        uint32_t old = _peer_initial_window_size;
                        _peer_initial_window_size = val;
                        int32_t delta = (int32_t)_peer_initial_window_size - (int32_t)old;
                        for (auto& kv : _streams) kv.second.send_window += delta;
                    } else if (id == SETTINGS_MAX_FRAME_SIZE) {
                        // RFC: allowed 16384..16777215
                        if (val < 16384) val = 16384;
                        if (val > 16777215u) val = 16777215u;
                        _peer_max_frame_size = val;
                    }
                }
                // flow-control related settings may unblock sending
                pump_all_streams();
                // reply ACK
                std::string ack = make_settings_ack();
                put_send_buf(new std::string(std::move(ack)));
                _got_client_settings = true;
            }
        } else if (type == PING) {
            // echo with ACK if length==8
            if (len == 8) {
                std::string hdr = make_frame_header(8, PING, FLAGS_ACK, 0);
                std::string frame; frame.reserve(9+8);
                frame += hdr; frame.append((const char*)payload, 8);
                put_send_buf(new std::string(std::move(frame)));
            }
        } else if (type == PRIORITY) {
            if (sid == 0 || len < 5) throw CMyCommonException("http2: PRIORITY invalid");
            uint32_t dep = read32(payload) & 0x7fffffffu; uint8_t w = payload[4];
            StreamState& st = _streams[sid]; st.dependency = dep; st.weight = (uint8_t)(w + 1);
        } else if (type == WINDOW_UPDATE) {
            if (len != 4) throw CMyCommonException("http2: WINDOW_UPDATE len");
            uint32_t inc = read32(payload) & 0x7fffffffu; if (inc == 0) throw CMyCommonException("http2: WINDOW_UPDATE zero");
            if (sid == 0) { _conn_send_window += (int32_t)inc; }
            else { _streams[sid].send_window += (int32_t)inc; }
            // attempt to send pending data after window increases
            if (sid == 0) pump_all_streams(); else try_send_data(sid);
            #ifdef DEBUG
            PDEBUG("[h2] WINDOW_UPDATE sid=%u inc=%u conn_win=%d", sid, inc, _conn_send_window);
            #endif
        } else if (type == RST_STREAM) {
            _streams.erase(sid);
        } else if (type == HEADERS || type == CONTINUATION) {
            uint32_t sid_valid = sid;
            if (sid_valid == 0) { throw CMyCommonException("http2: HEADERS with stream_id=0"); }
            const unsigned char* pld = payload;
            uint32_t remain = len;
            if (type == HEADERS) {
                // Skip PADDED/PRIORITY fields if present
                if (flags & FLAG_PADDED) { if (remain < 1) throw CMyCommonException("http2: PADDED with short payload"); uint8_t pad = *pld; ++pld; --remain; if (pad > remain) throw CMyCommonException("http2: invalid pad len"); remain -= pad; }
                if (flags & FLAG_PRIORITY) {
                    if (remain < 5) throw CMyCommonException("http2: PRIORITY short");
                    pld += 5; remain -= 5;
                }
                _assembling = true; _assembling_sid = sid_valid; _assembling_block.assign((const char*)pld, remain);
            } else { // CONTINUATION
                if (!_assembling || _assembling_sid != sid_valid) throw CMyCommonException("http2: CONTINUATION state");
                _assembling_block.append((const char*)pld, remain);
            }
            if (flags & FLAG_END_HEADERS) {
                bool end_stream = (type == HEADERS) && (flags & FLAG_END_STREAM);
                std::string blk = std::move(_assembling_block);
                _assembling = false; _assembling_sid = 0; _assembling_block.clear();
                (void)handle_headers_block(sid_valid, blk, end_stream);
#ifdef DEBUG
                PDEBUG("[h2] HEADERS_DONE stream=%u end_stream=%d size=%u", sid_valid, end_stream ? 1 : 0, (unsigned)blk.size());
#endif
            }
        } else if (type == DATA) {
            if (sid == 0) throw CMyCommonException("http2: DATA on stream 0");
            // padding if PADDED flag
            const unsigned char* pld = payload; uint32_t remain = len; uint32_t padlen = 0;
            if (flags & FLAG_PADDED) { if (remain < 1) throw CMyCommonException("http2: DATA padded short"); padlen = *pld; ++pld; --remain; if (padlen > remain) throw CMyCommonException("http2: DATA pad too long"); remain -= padlen; }
            on_data(sid, pld, remain, (flags & FLAG_END_STREAM));
            // flow control: add windows back for both stream and connection
            if (remain > 0) {
                std::string wu1 = make_window_update(0, remain);
                std::string wu2 = make_window_update(sid, remain);
                put_send_buf(new std::string(std::move(wu1)));
                put_send_buf(new std::string(std::move(wu2)));
            }
        }
        off += 9 + len;
    }
    consumed = off;
    return false;
}

size_t http2_process::process_recv_buf(const char* buf, size_t len) {
    // lazy-send server settings on first activity
    on_connected_once();

    if (len > 0) {
        _in.insert(_in.end(), (const unsigned char*)buf, (const unsigned char*)buf + len);
    }

    size_t consumed_total = 0;
    // Handle connection preface
    if (!_preface_ok) {
        if (_in.size() < CONNECTION_PREFACE_LEN) return 0;
        if (std::memcmp(_in.data(), CONNECTION_PREFACE, CONNECTION_PREFACE_LEN) != 0) {
            throw CMyCommonException("http2: bad connection preface");
        }
        _preface_ok = true;
        _in.erase(_in.begin(), _in.begin() + CONNECTION_PREFACE_LEN);
        consumed_total += CONNECTION_PREFACE_LEN;
    }

    // Parse frames
    size_t consumed = 0;
    bool sent_goaway = parse_frames(consumed);
    if (consumed) {
        _in.erase(_in.begin(), _in.begin() + consumed);
        consumed_total += consumed;
    }
    return consumed_total;
}

bool http2_process::handle_headers_block(uint32_t stream_id, const std::string& block, bool end_stream) {
    using namespace hpack;
    // decode HPACK block
    const unsigned char* p = (const unsigned char*)block.data();
    const unsigned char* end = p + block.size();
    std::vector<std::pair<std::string,std::string>> headers;
    while (p < end) {
        uint8_t b = *p;
        if (b & 0x80) {
            // Indexed Header Field Representation - 7-bit prefix
            uint32_t idx = 0; if (!decode_integer(p, end, 7, idx)) return false;
            auto& tbl = static_table(); if (idx == 0 || idx > tbl.size()) return false;
            headers.emplace_back(tbl[idx-1].name, tbl[idx-1].value);
        } else if ((b & 0x40) == 0x40) {
            // Literal Header Field with Incremental Indexing - 6-bit name index or 0 then string
            uint32_t nameIndex = 0; if (!decode_integer(p, end, 6, nameIndex)) return false;
            std::string name, value;
            if (nameIndex) {
                auto& tbl = static_table(); if (nameIndex == 0 || nameIndex > tbl.size()) return false;
                name = tbl[nameIndex-1].name;
            } else {
                if (!decode_string(p, end, name)) return false;
            }
            if (!decode_string(p, end, value)) return false;
            headers.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0xF0) == 0x00) {
            // Literal Header Field without Indexing - 4-bit name index or 0 then string
            uint32_t nameIndex = 0; if (!decode_integer(p, end, 4, nameIndex)) return false;
            std::string name, value;
            if (nameIndex) {
                auto& tbl = static_table(); if (nameIndex == 0 || nameIndex > tbl.size()) return false;
                name = tbl[nameIndex-1].name;
            } else {
                if (!decode_string(p, end, name)) return false;
            }
            if (!decode_string(p, end, value)) return false;
            headers.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0xF0) == 0x10) {
            // Literal Header Field Never Indexed - same as without indexing
            uint32_t nameIndex = 0; if (!decode_integer(p, end, 4, nameIndex)) return false;
            std::string name, value;
            if (nameIndex) {
                auto& tbl = static_table(); if (nameIndex == 0 || nameIndex > tbl.size()) return false;
                name = tbl[nameIndex-1].name;
            } else {
                if (!decode_string(p, end, name)) return false;
            }
            if (!decode_string(p, end, value)) return false;
            headers.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0xE0) == 0x20) {
            // Dynamic Table Size Update - 5-bit integer
            uint32_t dummy = 0; if (!decode_integer(p, end, 5, dummy)) return false;
        } else {
            return false;
        }
    }

    // Validate pseudo-header ordering and populate per-stream state
    auto itst = _streams.find(stream_id);
    if (itst == _streams.end()) {
        StreamState init;
        init.send_window = (int32_t)_peer_initial_window_size; // initialize per current peer setting
        itst = _streams.emplace(stream_id, std::move(init)).first;
    }
    StreamState& st = itst->second;
    bool seen_regular = false;
    bool seen_method = !st.method.empty();
    bool seen_path   = !st.path.empty();
    for (auto& kv : headers) {
        bool is_pseudo = !kv.first.empty() && kv.first[0] == ':';
        if (!is_pseudo) seen_regular = true;
        if (is_pseudo && seen_regular) {
            // Pseudo-header after regular header: RST_STREAM (stream-level error)
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_buf(new std::string(std::move(rst)));
            _streams.erase(stream_id);
            PDEBUG("[h2] RST_STREAM stream=%u reason=pseudo-after-regular", stream_id);
            return true;
        }
        if (kv.first == ":method") { if (seen_method) { std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR); put_send_buf(new std::string(std::move(rst))); _streams.erase(stream_id); PDEBUG("[h2] RST_STREAM stream=%u reason=dup-:method", stream_id); return true; } st.method = kv.second; seen_method = true; }
        else if (kv.first == ":path") { if (seen_path) { std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR); put_send_buf(new std::string(std::move(rst))); _streams.erase(stream_id); PDEBUG("[h2] RST_STREAM stream=%u reason=dup-:path", stream_id); return true; } st.path = kv.second; seen_path = true; }
        else if (kv.first == ":authority") st.authority = kv.second;
        else if (!is_pseudo) st.headers[kv.first] = kv.second;
#ifdef DEBUG
        if (is_pseudo)
            PDEBUG("[h2][HPACK] stream=%u pseudo '%s'='%s'", stream_id, kv.first.c_str(), kv.second.c_str());
        else
            PDEBUG("[h2][HPACK] stream=%u header '%s'='%s'", stream_id, kv.first.c_str(), kv.second.c_str());
#endif
    }
    // Validate regular headers: lowercase + forbidden connection-specific headers
    static const char* kBadHdrs[] = { "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade" };
    for (auto& kvh : st.headers) {
        for (char c : kvh.first) {
            if (c >= 'A' && c <= 'Z') {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_buf(new std::string(std::move(rst)));
                _streams.erase(stream_id);
                PDEBUG("[h2] RST_STREAM stream=%u reason=uppercase-header '%s'", stream_id, kvh.first.c_str());
                return true;
            }
        }
        for (const char* bad : kBadHdrs) {
            if (kvh.first == bad) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_buf(new std::string(std::move(rst)));
                _streams.erase(stream_id);
                PDEBUG("[h2] RST_STREAM stream=%u reason=forbidden-header '%s'", stream_id, kvh.first.c_str());
                return true;
            }
        }
    }

    // CONNECT vs non-CONNECT required pseudo-headers
    if (!st.method.empty()) {
        std::string m = st.method; for (auto& c : m) c = (char)tolower(c);
        if (m == "connect") {
            if (!st.path.empty()) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_buf(new std::string(std::move(rst)));
                _streams.erase(stream_id);
                PDEBUG("[h2] RST_STREAM stream=%u reason=CONNECT-has-:path", stream_id);
                return true;
            }
            if (st.authority.empty()) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_buf(new std::string(std::move(rst)));
                _streams.erase(stream_id);
                PDEBUG("[h2] RST_STREAM stream=%u reason=CONNECT-no-:authority", stream_id);
                return true;
            }
        } else {
            if (st.path.empty()) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_buf(new std::string(std::move(rst)));
                _streams.erase(stream_id);
                PDEBUG("[h2] RST_STREAM stream=%u reason=no-:path", stream_id);
                return true;
            }
        }
    }
    if (end_stream) finish_stream(stream_id);
    return true;
}

void http2_process::send_response(uint32_t stream_id, const HttpResponse& rsp) {
    using namespace hpack;
    // Minimal response: status + content-type + content-length + body
    std::string body = rsp.body;
    std::string block;
    // Encode :status 200
    // Literal without indexing, name by index if available
    {
        uint32_t name_idx = static_index_of_name(":status");
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) { encode_string(block, std::string(":status"), true); }
        encode_string(block, std::to_string(rsp.status), true);
    }
    // content-type
    {
        uint32_t name_idx = static_index_of_name("content-type");
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) { encode_string(block, std::string("content-type"), true); }
        auto it = rsp.headers.find("Content-Type");
        encode_string(block, it != rsp.headers.end() ? it->second : std::string("text/plain"), true);
    }
    // content-length
    {
        uint32_t name_idx = static_index_of_name("content-length");
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) { encode_string(block, std::string("content-length"), true); }
        encode_string(block, std::to_string(body.size()), true);
    }
    // other headers (optional, non-pseudo)
    for (auto& kv : rsp.headers) {
        if (kv.first == "Content-Type" || kv.first == "content-type" || kv.first == "content-length") continue;
        // HTTP/2 requires lowercase header field-names. Transform name to lowercase.
        std::string lname = kv.first; for (auto& c : lname) c = (char)tolower(c);
        uint32_t name_idx = static_index_of_name(lname);
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) { encode_string(block, lname, true); }
        encode_string(block, kv.second, true);
    }
    // HEADERS frame with END_HEADERS (use Huffman for strings), no END_STREAM here
    std::string hdr = make_frame_header((uint32_t)block.size(), HEADERS, FLAG_END_HEADERS, stream_id);
    std::string frame = hdr + block;
    put_send_buf(new std::string(std::move(frame)));

    // If no body, send empty DATA with END_STREAM; else enqueue body and try to send within flow control windows
    StreamState& st = _streams[stream_id];
    if (body.empty()) {
        std::string datahdr = make_frame_header(0, DATA, FLAG_END_STREAM, stream_id);
        put_send_buf(new std::string(std::move(datahdr)));
    } else {
        st.out_body = std::move(body);
        st.out_off = 0;
        try_send_data(stream_id);
    }
}

void http2_process::on_data(uint32_t stream_id, const unsigned char* p, uint32_t len, bool end_stream) {
    if (len) _streams[stream_id].body.append((const char*)p, (size_t)len);
    if (end_stream) finish_stream(stream_id);
}

void http2_process::finish_stream(uint32_t stream_id) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end()) { return; }
    StreamState st = std::move(it->second);
    _streams.erase(it);

    HttpRequest req; req.version = "HTTP/2";
    req.method = st.method.empty() ? "GET" : st.method;
    req.url = st.path.empty() ? "/" : st.path;
    req.body = std::move(st.body);
    if (!st.authority.empty()) req.headers["host"] = st.authority;
    for (auto& kv : st.headers) req.headers[kv.first] = kv.second;

    HttpResponse rsp; rsp.status = 200; rsp.reason = "OK";
    if (_app) _app->on_http(req, rsp);
    if (rsp.headers.find("Content-Type") == rsp.headers.end()) rsp.set_content_type("text/plain");
    if (rsp.body.empty()) rsp.body = "OK";
    PDEBUG("[h2] stream=%u %s %s body=%zu", stream_id, req.method.c_str(), req.url.c_str(), req.body.size());
    send_response(stream_id, rsp);
}

void http2_process::try_send_data(uint32_t stream_id) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end()) return;
    StreamState& st = it->second;
    if (st.out_off >= st.out_body.size()) return;
    // loop to send multiple frames within allowance to reduce wakeups
    int frames_sent = 0;
    const int kMaxFramesPerPump = 8;
    while (st.out_off < st.out_body.size() && _conn_send_window > 0 && st.send_window > 0 && frames_sent < kMaxFramesPerPump) {
        uint32_t remaining = (uint32_t)(st.out_body.size() - st.out_off);
        uint32_t allowance = (uint32_t)std::min<int32_t>(_conn_send_window, st.send_window);
        allowance = std::min<uint32_t>(allowance, _peer_max_frame_size);
        if (allowance == 0) break;
        uint32_t chunk = std::min<uint32_t>(allowance, remaining);
        std::string payload = st.out_body.substr(st.out_off, chunk);
        st.out_off += chunk;
        _conn_send_window -= (int32_t)chunk;
        st.send_window -= (int32_t)chunk;
        uint8_t fl = (st.out_off >= st.out_body.size()) ? FLAG_END_STREAM : 0;
        std::string hdr = make_frame_header(chunk, DATA, fl, stream_id);
        std::string frame = hdr + payload;
        put_send_buf(new std::string(std::move(frame)));
#ifdef DEBUG
        PDEBUG("[h2] SEND DATA stream=%u chunk=%u conn_win=%d stream_win=%d end=%d", stream_id, chunk, _conn_send_window, st.send_window, fl ? 1 : 0);
#endif
        frames_sent++;
    }
}

void http2_process::pump_all_streams() {
    if (_streams.empty()) return;
    // build a stable snapshot of stream ids
    std::vector<uint32_t> ids; ids.reserve(_streams.size());
    for (auto& kv : _streams) ids.push_back(kv.first);
    size_t n = ids.size(); if (n == 0) return;
    size_t start = (_send_rr++) % n;
    for (size_t i = 0; i < n; ++i) {
        uint32_t sid = ids[(start + i) % n];
        try_send_data(sid);
    }
}
