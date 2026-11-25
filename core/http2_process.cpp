#include "http2_process.h"
#include "common_exception.h"
#include "protocol_limits.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include "hpack.h"
using namespace h2;

static const uint8_t FLAG_END_STREAM  = 0x1;
static const uint8_t FLAG_END_HEADERS = 0x4;
static const uint8_t FLAG_PADDED      = 0x8;
static const uint8_t FLAG_PRIORITY    = 0x20;

void http2_process::send_goaway_and_throw(ErrorCode ec, const char* msg) {
    std::string go = make_goaway(_last_stream_id, ec, msg ? std::string(msg) : std::string());
    put_send_move(std::move(go));
    throw CMyCommonException(msg ? msg : "http2 protocol error");
}

void http2_process::on_connected_once() {
    if (_sent_settings) return;
    // Advertise server SETTINGS: ENABLE_PUSH=0, MAX_CONCURRENT_STREAMS=100
    std::string payload;
    payload.reserve(12);
    // SETTINGS_ENABLE_PUSH = 0
    payload.push_back(static_cast<char>(0x00));
    payload.push_back(static_cast<char>(SETTINGS_ENABLE_PUSH));
    write32(payload, 0);
    // SETTINGS_MAX_CONCURRENT_STREAMS = kMaxConcurrentStreams
    payload.push_back(static_cast<char>(0x00));
    payload.push_back(static_cast<char>(SETTINGS_MAX_CONCURRENT_STREAMS));
    write32(payload, kMaxConcurrentStreams);
    std::string settings = make_frame_header(static_cast<uint32_t>(payload.size()), SETTINGS, 0, 0);
    settings += payload;
    put_send_move(std::move(settings));
    _sent_settings = true;
}

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

        // Enforce local SETTINGS_MAX_FRAME_SIZE (RFC 9113 4.2) before waiting for payload.
        if (len > kLocalMaxFrameSize) {
            send_goaway_and_throw(FRAME_SIZE_ERROR, "http2: frame exceeds local max frame size");
        }
        if (type == PING && (len != 8 || sid != 0)) {
            send_goaway_and_throw(sid != 0 ? PROTOCOL_ERROR : FRAME_SIZE_ERROR, "http2: invalid PING");
        }
        if (type == SETTINGS && (sid != 0 || ((flags & FLAGS_ACK) ? len != 0 : (len % 6) != 0))) {
            send_goaway_and_throw(sid != 0 ? PROTOCOL_ERROR : FRAME_SIZE_ERROR, "http2: invalid SETTINGS");
        }
        if (type == WINDOW_UPDATE && len != 4) {
            send_goaway_and_throw(FRAME_SIZE_ERROR, "http2: invalid WINDOW_UPDATE length");
        }
        if (type == RST_STREAM && (sid == 0 || len != 4)) {
            send_goaway_and_throw(sid == 0 ? PROTOCOL_ERROR : FRAME_SIZE_ERROR, "http2: invalid RST_STREAM");
        }
        if (type == PRIORITY && (sid == 0 || len != 5)) {
            send_goaway_and_throw(sid == 0 ? PROTOCOL_ERROR : FRAME_SIZE_ERROR, "http2: invalid PRIORITY");
        }
        if (type == GOAWAY && (sid != 0 || len < 8)) {
            send_goaway_and_throw(sid != 0 ? PROTOCOL_ERROR : FRAME_SIZE_ERROR, "http2: invalid GOAWAY");
        }

        if (n - off < 9 + len) break; // incomplete frame

        // RFC 9113 6.10: while assembling a header block, only CONTINUATION on the same stream is legal.
        if (_assembling && (type != CONTINUATION || sid != _assembling_sid)) {
            send_goaway_and_throw(PROTOCOL_ERROR, "http2: expected CONTINUATION frame");
        }

        const unsigned char* payload = p + 9;
        if (type == SETTINGS) {
            if ((flags & FLAGS_ACK) == 0) {
                for (uint32_t offp = 0; offp + 6 <= len; offp += 6) {
                    uint16_t id = (static_cast<uint16_t>(payload[offp]) << 8) | payload[offp + 1];
                    uint32_t val = read32(payload + offp + 2);
                    if (id == SETTINGS_INITIAL_WINDOW_SIZE) {
                        if (val > 0x7fffffffu) {
                            send_goaway_and_throw(FLOW_CONTROL_ERROR, "http2: INITIAL_WINDOW_SIZE overflow");
                        }
                        int64_t delta = static_cast<int64_t>(val) - static_cast<int64_t>(_peer_initial_window_size);
                        _peer_initial_window_size = val;
                        for (auto& kv : _streams) {
                            int64_t nw = static_cast<int64_t>(kv.second.send_window) + delta;
                            if (nw > 0x7fffffffLL) {
                                send_goaway_and_throw(FLOW_CONTROL_ERROR, "http2: stream send window overflow");
                            }
                            kv.second.send_window = static_cast<int32_t>(nw);
                        }
                    } else if (id == SETTINGS_MAX_FRAME_SIZE) {
                        if (val < 16384u || val > 16777215u) {
                            send_goaway_and_throw(PROTOCOL_ERROR, "http2: invalid MAX_FRAME_SIZE");
                        }
                        _peer_max_frame_size = val;
                    } else if (id == SETTINGS_HEADER_TABLE_SIZE) {
                        _enc_tbl_max = val;
                        enc_dyn_evict();
                    }
                }
                pump_all_streams();
                std::string ack = make_settings_ack();
                put_send_move(std::move(ack));
                _got_client_settings = true;
            }
        } else if (type == PING) {
            // Only echo PING when ACK flag is NOT set (RFC 9113 6.7).
            if ((flags & FLAGS_ACK) == 0) {
                std::string hdr = make_frame_header(8, PING, FLAGS_ACK, 0);
                std::string frame;
                frame.reserve(9 + 8);
                frame += hdr;
                frame.append(reinterpret_cast<const char*>(payload), 8);
                put_send_move(std::move(frame));
            }
        } else if (type == PRIORITY) {
            uint32_t dep = read32(payload) & 0x7fffffffu;
            uint8_t w = payload[4];
            if (dep == sid) {
                std::string rst = make_rst_stream(sid, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(sid);
            } else {
                // Only update already-active streams; never materialize idle streams via PRIORITY.
                auto it = _streams.find(sid);
                if (it != _streams.end()) {
                    it->second.dependency = dep;
                    it->second.weight = static_cast<uint8_t>(w + 1);
                    update_quantum(it->second);
                }
            }
        } else if (type == WINDOW_UPDATE) {
            uint32_t inc = read32(payload) & 0x7fffffffu;
            if (inc == 0) {
                if (sid == 0) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: zero connection WINDOW_UPDATE");
                } else {
                    std::string rst = make_rst_stream(sid, PROTOCOL_ERROR);
                    put_send_move(std::move(rst));
                    _streams.erase(sid);
                }
            } else if (sid == 0) {
                int64_t nw = static_cast<int64_t>(_conn_send_window) + static_cast<int64_t>(inc);
                if (nw > 0x7fffffffLL) {
                    send_goaway_and_throw(FLOW_CONTROL_ERROR, "http2: connection send window overflow");
                }
                _conn_send_window = static_cast<int32_t>(nw);
                pump_all_streams();
            } else {
                auto it = _streams.find(sid);
                if (it != _streams.end()) {
                    int64_t nw = static_cast<int64_t>(it->second.send_window) + static_cast<int64_t>(inc);
                    if (nw > 0x7fffffffLL) {
                        std::string rst = make_rst_stream(sid, FLOW_CONTROL_ERROR);
                        put_send_move(std::move(rst));
                        _streams.erase(it);
                    } else {
                        it->second.send_window = static_cast<int32_t>(nw);
                        try_send_data(sid);
                    }
                } else if (sid > _last_stream_id) {
                    // RFC 9113 5.1: WINDOW_UPDATE on an idle stream is PROTOCOL_ERROR.
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: WINDOW_UPDATE on idle stream");
                }
            }
        } else if (type == RST_STREAM) {
            if (sid > _last_stream_id && _streams.find(sid) == _streams.end()) {
                send_goaway_and_throw(PROTOCOL_ERROR, "http2: RST_STREAM on idle stream");
            }
            _streams.erase(sid);
        } else if (type == GOAWAY) {
            // Peer initiated shutdown; stop accepting new work quietly.
            uint32_t peer_last_sid = read32(payload) & 0x7fffffffu;
            uint32_t err = read32(payload + 4);
            PDEBUG("[h2] recv GOAWAY last_stream_id=%u error=0x%x", peer_last_sid, (unsigned)err);
        } else if (type == HEADERS || type == CONTINUATION) {
            if (sid == 0) {
                send_goaway_and_throw(PROTOCOL_ERROR, "http2: HEADERS with stream_id=0");
            }
            const unsigned char* pld = payload;
            uint32_t remain = len;
            bool self_dep = false;
            if (type == HEADERS) {
                if ((sid & 1u) == 0) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: client used even stream_id");
                }
                if (flags & FLAG_PADDED) {
                    if (remain < 1) send_goaway_and_throw(FRAME_SIZE_ERROR, "http2: PADDED HEADERS short");
                    uint8_t pad = *pld++; --remain;
                    if (pad > remain) send_goaway_and_throw(PROTOCOL_ERROR, "http2: invalid HEADERS pad len");
                    remain -= pad;
                }
                if (flags & FLAG_PRIORITY) {
                    if (remain < 5) send_goaway_and_throw(FRAME_SIZE_ERROR, "http2: PRIORITY HEADERS short");
                    uint32_t dep = read32(pld) & 0x7fffffffu;
                    if (dep == sid) self_dep = true;
                    pld += 5; remain -= 5;
                }
                if (remain > MAX_HTTP_HEAD_LEN) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: header block too large");
                }
                _assembling = true;
                _assembling_sid = sid;
                _assembling_end_stream = (flags & FLAG_END_STREAM) != 0;
                _assembling_self_dep = self_dep;
                _assembling_block.assign(reinterpret_cast<const char*>(pld), remain);
            } else { // CONTINUATION
                if (!_assembling || _assembling_sid != sid) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: unexpected CONTINUATION");
                }
                if (remain > MAX_HTTP_HEAD_LEN - _assembling_block.size()) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: assembled header block exceeds limit");
                }
                _assembling_block.append(reinterpret_cast<const char*>(pld), remain);
            }
            if (flags & FLAG_END_HEADERS) {
                bool end_stream = _assembling_end_stream;
                bool reject_self_dep = _assembling_self_dep;
                std::string blk = std::move(_assembling_block);
                _assembling = false;
                _assembling_sid = 0;
                _assembling_end_stream = false;
                _assembling_self_dep = false;
                _assembling_block.clear();
                if (!handle_headers_block(sid, blk, reject_self_dep ? false : end_stream)) {
                    send_goaway_and_throw(COMPRESSION_ERROR, "http2: HPACK decode error");
                }
                if (reject_self_dep) {
                    std::string rst = make_rst_stream(sid, PROTOCOL_ERROR);
                    put_send_move(std::move(rst));
                    _streams.erase(sid);
                }
#ifdef DEBUG
                PDEBUG("[h2] HEADERS_DONE stream=%u end_stream=%d size=%u", sid, end_stream ? 1 : 0, (unsigned)blk.size());
#endif
            }
        } else if (type == DATA) {
            if (sid == 0) {
                send_goaway_and_throw(PROTOCOL_ERROR, "http2: DATA on stream 0");
            }
            const unsigned char* pld = payload;
            uint32_t remain = len;
            if (flags & FLAG_PADDED) {
                if (remain < 1) send_goaway_and_throw(FRAME_SIZE_ERROR, "http2: DATA padded short");
                uint8_t padlen = *pld++; --remain;
                if (padlen > remain) send_goaway_and_throw(PROTOCOL_ERROR, "http2: DATA pad too long");
                remain -= padlen;
            }
            const bool end_stream = (flags & FLAG_END_STREAM) != 0;
            // RFC 9113 6.1: flow-control window accounts for the entire DATA frame payload (incl. padding).
            if (len > 0) {
                std::string wu_conn = make_window_update(0, len);
                put_send_move(std::move(wu_conn));
            }
            auto it = _streams.find(sid);
            if (it == _streams.end()) {
                if (sid > _last_stream_id) {
                    send_goaway_and_throw(PROTOCOL_ERROR, "http2: DATA on idle stream");
                }
                std::string rst = make_rst_stream(sid, STREAM_CLOSED);
                put_send_move(std::move(rst));
            } else if (!it->second.headers_received || it->second.end_stream_received) {
                std::string rst = make_rst_stream(sid, STREAM_CLOSED);
                put_send_move(std::move(rst));
                _streams.erase(it);
            } else {
                const size_t body_cap = myframe::max_http_body_bytes();
                if (remain > body_cap || it->second.body.size() > body_cap - remain) {
                    std::string rst = make_rst_stream(sid, CANCEL);
                    put_send_move(std::move(rst));
                    _streams.erase(it);
                } else {
                    if (len > 0 && !end_stream) {
                        std::string wu_strm = make_window_update(sid, len);
                        put_send_move(std::move(wu_strm));
                    }
                    on_data(sid, pld, remain, end_stream);
                }
            }
        }
        off += 9 + len;
    }
    consumed = off;
    return false;
}

size_t http2_process::process_recv_buf(const char* buf, size_t len) {
    on_connected_once();

    if (len > 0) {
        _in.insert(_in.end(), reinterpret_cast<const unsigned char*>(buf),
                   reinterpret_cast<const unsigned char*>(buf) + len);
    }

    if (!_preface_ok) {
        if (_in.size() < CONNECTION_PREFACE_LEN) {
            if (!_in.empty() && std::memcmp(_in.data(), CONNECTION_PREFACE, _in.size()) != 0) {
                throw CMyCommonException("http2: bad connection preface");
            }
            return len;
        }
        if (std::memcmp(_in.data(), CONNECTION_PREFACE, CONNECTION_PREFACE_LEN) != 0) {
            throw CMyCommonException("http2: bad connection preface");
        }
        _preface_ok = true;
        _in.erase(_in.begin(), _in.begin() + CONNECTION_PREFACE_LEN);
    }

    size_t consumed = 0;
    (void)parse_frames(consumed);
    if (consumed) {
        _in.erase(_in.begin(), _in.begin() + consumed);
    }
    return len;
}

void http2_process::dec_dyn_evict() {
    while (_dec_tbl_size > _dec_tbl_max && !_dec_dyn.empty()) {
        const DynHdr& last = _dec_dyn.back();
        if (_dec_tbl_size >= last.sz) _dec_tbl_size -= last.sz; else _dec_tbl_size = 0;
        _dec_dyn.pop_back();
    }
}

void http2_process::dec_dyn_add(const std::string& name, const std::string& value) {
    const size_t sz = 32u + name.size() + value.size();
    if (sz > _dec_tbl_max) {
        _dec_dyn.clear();
        _dec_tbl_size = 0;
        return;
    }
    while (_dec_tbl_size + sz > _dec_tbl_max && !_dec_dyn.empty()) {
        const DynHdr& last = _dec_dyn.back();
        if (_dec_tbl_size >= last.sz) _dec_tbl_size -= last.sz; else _dec_tbl_size = 0;
        _dec_dyn.pop_back();
    }
    DynHdr e;
    e.name = name;
    e.value = value;
    e.sz = sz;
    _dec_dyn.push_front(std::move(e));
    _dec_tbl_size += sz;
}

bool http2_process::dec_lookup(uint32_t idx, std::string& name, std::string& value) const {
    const std::vector<hpack::HeaderField>& stbl = hpack::static_table();
    if (idx == 0) return false;
    if (idx <= stbl.size()) {
        name = stbl[idx - 1].name;
        value = stbl[idx - 1].value;
        return true;
    }
    const size_t di = static_cast<size_t>(idx) - stbl.size() - 1u;
    if (di >= _dec_dyn.size()) return false;
    name = _dec_dyn[di].name;
    value = _dec_dyn[di].value;
    return true;
}

bool http2_process::handle_headers_block(uint32_t stream_id, const std::string& block, bool end_stream) {
    using namespace hpack;
    // 1) Decode the HPACK block FIRST so decoder dynamic table stays synchronized
    //    even if the stream itself is refused or reset (RFC 9113 4.3).
    const unsigned char* p = reinterpret_cast<const unsigned char*>(block.data());
    const unsigned char* end = p + block.size();
    std::vector<std::pair<std::string, std::string> > headers;
    bool allow_size_update = true;
    while (p < end) {
        uint8_t b = *p;
        if (b & 0x80u) {
            // Indexed Header Field Representation (RFC 7541 6.1)
            allow_size_update = false;
            uint32_t idx = 0;
            if (!decode_integer(p, end, 7, idx)) return false;
            std::string name, value;
            if (!dec_lookup(idx, name, value)) return false;
            headers.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0xC0u) == 0x40u) {
            // Literal Header Field with Incremental Indexing (RFC 7541 6.2.1)
            allow_size_update = false;
            uint32_t nameIndex = 0;
            if (!decode_integer(p, end, 6, nameIndex)) return false;
            std::string name, value;
            if (nameIndex) {
                std::string dummy_val;
                if (!dec_lookup(nameIndex, name, dummy_val)) return false;
            } else {
                if (!decode_string(p, end, name)) return false;
            }
            if (!decode_string(p, end, value)) return false;
            dec_dyn_add(name, value);
            headers.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0xE0u) == 0x20u) {
            // Dynamic Table Size Update (RFC 7541 6.3): only valid before any header field.
            if (!allow_size_update) return false;
            uint32_t new_max = 0;
            if (!decode_integer(p, end, 5, new_max)) return false;
            if (new_max > _dec_tbl_hard_max) return false;
            _dec_tbl_max = new_max;
            dec_dyn_evict();
        } else if ((b & 0xF0u) == 0x00u || (b & 0xF0u) == 0x10u) {
            // Literal without Indexing / Never Indexed (RFC 7541 6.2.2 & 6.2.3)
            allow_size_update = false;
            uint32_t nameIndex = 0;
            if (!decode_integer(p, end, 4, nameIndex)) return false;
            std::string name, value;
            if (nameIndex) {
                std::string dummy_val;
                if (!dec_lookup(nameIndex, name, dummy_val)) return false;
            } else {
                if (!decode_string(p, end, name)) return false;
            }
            if (!decode_string(p, end, value)) return false;
            headers.emplace_back(std::move(name), std::move(value));
        } else {
            return false;
        }
    }

    // 2) Stream state machine check (RFC 9113 5.1.1).
    auto itst = _streams.find(stream_id);
    bool is_trailers = false;
    if (itst == _streams.end()) {
        if (stream_id <= _last_stream_id) {
            // Closed stream receiving HEADERS -> connection error.
            send_goaway_and_throw(STREAM_CLOSED, "http2: HEADERS on closed stream");
        }
        _last_stream_id = stream_id;
        if (_streams.size() >= kMaxConcurrentStreams) {
            std::string rst = make_rst_stream(stream_id, REFUSED_STREAM);
            put_send_move(std::move(rst));
            return true;
        }
        StreamState init;
        init.send_window = static_cast<int32_t>(_peer_initial_window_size);
        init.headers_received = true;
        itst = _streams.emplace(stream_id, std::move(init)).first;
    } else {
        if (itst->second.end_stream_received) {
            std::string rst = make_rst_stream(stream_id, STREAM_CLOSED);
            put_send_move(std::move(rst));
            _streams.erase(itst);
            return true;
        }
        is_trailers = true;
        if (!end_stream) {
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_move(std::move(rst));
            _streams.erase(itst);
            return true;
        }
    }

    StreamState& st = itst->second;
    bool seen_regular = is_trailers; // Trailers must not contain pseudo-headers.
    bool seen_method = !st.method.empty();
    bool seen_path   = !st.path.empty();
    for (auto& kv : headers) {
        bool is_pseudo = !kv.first.empty() && kv.first[0] == ':';
        if (!is_pseudo) seen_regular = true;
        if (is_pseudo && seen_regular) {
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_move(std::move(rst));
            _streams.erase(stream_id);
            return true;
        }
        if (kv.first == ":method") {
            if (seen_method) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(stream_id);
                return true;
            }
            st.method = kv.second;
            seen_method = true;
        } else if (kv.first == ":path") {
            if (seen_path) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(stream_id);
                return true;
            }
            st.path = kv.second;
            seen_path = true;
        } else if (kv.first == ":authority") {
            st.authority = kv.second;
        } else if (kv.first == ":scheme") {
            // accepted pseudo-header
        } else if (is_pseudo) {
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_move(std::move(rst));
            _streams.erase(stream_id);
            return true;
        } else {
            st.headers[kv.first] = kv.second;
        }
    }

    // Validate regular headers: lowercase + forbidden connection-specific headers
    static const char* kBadHdrs[] = { "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade" };
    for (auto& kvh : st.headers) {
        for (char c : kvh.first) {
            if (c >= 'A' && c <= 'Z') {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(stream_id);
                return true;
            }
        }
        for (const char* bad : kBadHdrs) {
            if (kvh.first == bad) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(stream_id);
                return true;
            }
        }
    }

    if (!is_trailers) {
        if (st.method.empty()) {
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_move(std::move(rst));
            _streams.erase(stream_id);
            return true;
        }
        std::string m = st.method;
        for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (m == "connect") {
            if (!st.path.empty() || st.authority.empty()) {
                std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
                put_send_move(std::move(rst));
                _streams.erase(stream_id);
                return true;
            }
        } else if (st.path.empty()) {
            std::string rst = make_rst_stream(stream_id, PROTOCOL_ERROR);
            put_send_move(std::move(rst));
            _streams.erase(stream_id);
            return true;
        }
    }

    if (end_stream) {
        st.end_stream_received = true;
        finish_stream(stream_id);
    }
    return true;
}

static uint32_t h2_static_status_index(int status) {
    switch (status) {
        case 200: return 8;
        case 204: return 9;
        case 206: return 10;
        case 304: return 11;
        case 400: return 12;
        case 404: return 13;
        case 500: return 14;
        default:  return 0;
    }
}

void http2_process::send_response(uint32_t stream_id, const myframe::HttpResponse& rsp,
                                  bool suppress_body,
                                  int32_t initial_send_window) {
    using namespace hpack;
    std::string body = rsp.body;
    bool has_content_length = false;
    std::string content_length;
    bool has_content_type = false;
    std::string content_type;
    for (const auto& kv : rsp.headers) {
        if (strcasecmp(kv.first.c_str(), "content-length") == 0) {
            has_content_length = true;
            content_length = kv.second;
        } else if (strcasecmp(kv.first.c_str(), "content-type") == 0) {
            has_content_type = true;
            content_type = kv.second;
        }
    }
    const bool no_body_status = (rsp.status == 204 || rsp.status == 304);
    if (no_body_status) {
        suppress_body = true;
    }
    if (!has_content_length && !no_body_status && (!suppress_body || !body.empty())) {
        has_content_length = true;
        content_length = std::to_string(body.size());
    }
    if (suppress_body) body.clear();

    std::string block;
    // :status — use RFC 7541 static index for common codes, else literal without indexing.
    {
        uint32_t sidx = h2_static_status_index(rsp.status);
        if (sidx) {
            encode_integer(block, sidx, 7, 0x80);
        } else {
            uint32_t name_idx = static_index_of_name(":status");
            encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
            if (!name_idx) encode_string(block, std::string(":status"), false);
            encode_string(block, std::to_string(rsp.status), false);
        }
    }
    // content-type
    if (has_content_type || !no_body_status) {
        uint32_t name_idx = static_index_of_name("content-type");
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) encode_string(block, std::string("content-type"), false);
        encode_string(block, has_content_type ? content_type : std::string("text/plain"), false);
    }
    // content-length
    if (has_content_length) {
        uint32_t name_idx = static_index_of_name("content-length");
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) encode_string(block, std::string("content-length"), false);
        encode_string(block, content_length, false);
    }
    // Other response headers: always encode as Literal Header Field without Indexing
    // (0x00, 4-bit static name index) so encoder/decoder dynamic tables can never desync (N4b).
    for (const auto& kv : rsp.headers) {
        if (strcasecmp(kv.first.c_str(), "content-type") == 0 ||
            strcasecmp(kv.first.c_str(), "content-length") == 0 ||
            strcasecmp(kv.first.c_str(), "connection") == 0 ||
            strcasecmp(kv.first.c_str(), "transfer-encoding") == 0 ||
            strcasecmp(kv.first.c_str(), "keep-alive") == 0 ||
            strcasecmp(kv.first.c_str(), "upgrade") == 0) {
            continue;
        }
        std::string lname = kv.first;
        for (auto& c : lname) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const std::string& lval = kv.second;
        uint32_t name_idx = static_index_of_name(lname);
        encode_integer(block, name_idx ? name_idx : 0, 4, 0x00);
        if (!name_idx) encode_string(block, lname, false);
        encode_string(block, lval, false);
    }

    // Emit HEADERS (and CONTINUATION if block exceeds _peer_max_frame_size).
    const uint32_t max_frag = _peer_max_frame_size ? _peer_max_frame_size : 16384u;
    const uint8_t end_stream_flag = body.empty() ? FLAG_END_STREAM : 0;
    if (block.size() <= max_frag) {
        std::string hdr = make_frame_header(static_cast<uint32_t>(block.size()), HEADERS,
                                            static_cast<uint8_t>(FLAG_END_HEADERS | end_stream_flag),
                                            stream_id);
        std::string frame = hdr + block;
        put_send_move(std::move(frame));
    } else {
        size_t pos = 0;
        bool first = true;
        while (pos < block.size()) {
            size_t chunk = std::min<size_t>(max_frag, block.size() - pos);
            bool last = (pos + chunk >= block.size());
            FrameType ft = first ? HEADERS : CONTINUATION;
            uint8_t fl = (first ? end_stream_flag : 0) | (last ? FLAG_END_HEADERS : 0);
            std::string frame = make_frame_header(static_cast<uint32_t>(chunk), ft, fl, stream_id);
            frame.append(block.data() + pos, chunk);
            put_send_move(std::move(frame));
            pos += chunk;
            first = false;
        }
    }

    // Empty body is already complete via HEADERS(END_STREAM); do not retain stream state (N5).
    if (body.empty()) {
        _streams.erase(stream_id);
        return;
    }

    StreamState& st = _streams[stream_id];
    st.send_window = (initial_send_window >= 0) ? initial_send_window
                                                : static_cast<int32_t>(_peer_initial_window_size);
    st.end_stream_received = true;
    st.headers_received = true;
    st.out_body = std::move(body);
    st.out_off = 0;
    (void)try_send_data(stream_id);
}

void http2_process::on_data(uint32_t stream_id, const unsigned char* p, uint32_t len, bool end_stream) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end()) return;
    if (len) it->second.body.append(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
    if (end_stream) {
        it->second.end_stream_received = true;
        finish_stream(stream_id);
    }
}

void http2_process::finish_stream(uint32_t stream_id) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end()) { return; }
    StreamState st = std::move(it->second);
    _streams.erase(it);

    myframe::HttpRequest req;
    req.version = "HTTP/2";
    req.method = st.method.empty() ? "GET" : st.method;
    req.url = st.path.empty() ? "/" : st.path;
    req.body = std::move(st.body);
    if (!st.authority.empty()) req.headers["host"] = st.authority;
    for (auto& kv : st.headers) req.headers[kv.first] = kv.second;

    myframe::HttpResponse rsp;
    rsp.status = 200;
    rsp.reason = "OK";
    if (_app) {
        myframe::detail::HandlerContextScope scope(this);
        _app->on_http(req, rsp);
    } else if (rsp.body.empty()) {
        rsp.body = "OK";
    }
    const bool head_only = strcasecmp(req.method.c_str(), "HEAD") == 0;
    PDEBUG("[h2] stream=%u %s %s body=%zu", stream_id, req.method.c_str(), req.url.c_str(), req.body.size());
    send_response(stream_id, rsp, head_only, st.send_window);
}

void http2_process::handle_msg(std::shared_ptr<normal_msg>& msg) {
    if (!_app) return;
    if (!_app->handle_thread_msg(msg)) {
        _app->handle_msg(msg);
    }
}

void http2_process::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (_app) { _app->handle_timeout(t_msg); }
}

uint32_t http2_process::try_send_data(uint32_t stream_id) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end()) return 0;
    StreamState& st = it->second;
    if (st.out_off >= st.out_body.size()) {
        if (st.end_stream_received && !st.out_body.empty()) {
            _streams.erase(it);
        }
        return 0;
    }
    uint32_t total_sent = 0;
    while (st.out_off < st.out_body.size() && _conn_send_window > 0 && st.send_window > 0) {
        uint32_t remaining = static_cast<uint32_t>(st.out_body.size() - st.out_off);
        uint32_t allowance = static_cast<uint32_t>(std::min<int32_t>(_conn_send_window, st.send_window));
        allowance = std::min<uint32_t>(allowance, _peer_max_frame_size);
        if (allowance == 0) break;
        uint32_t chunk = std::min<uint32_t>(allowance, remaining);
        std::string payload = st.out_body.substr(st.out_off, chunk);
        st.out_off += chunk;
        _conn_send_window -= static_cast<int32_t>(chunk);
        st.send_window -= static_cast<int32_t>(chunk);
        uint8_t fl = (st.out_off >= st.out_body.size()) ? FLAG_END_STREAM : 0;
        std::string hdr = make_frame_header(chunk, DATA, fl, stream_id);
        std::string frame = hdr + payload;
        put_send_move(std::move(frame));
#ifdef DEBUG
        PDEBUG("[h2] SEND DATA stream=%u chunk=%u conn_win=%d stream_win=%d end=%d", stream_id, chunk, _conn_send_window, st.send_window, fl ? 1 : 0);
#endif
        total_sent += chunk;
    }
    // Once final DATA(END_STREAM) has been queued, release stream state and response buffer (N5).
    if (st.out_off >= st.out_body.size()) {
        _streams.erase(it);
    }
    return total_sent;
}

void http2_process::pump_all_streams() {
    if (_streams.empty()) return;
    std::vector<uint32_t> ids;
    ids.reserve(_streams.size());
    for (auto& kv : _streams) ids.push_back(kv.first);
    size_t n = ids.size();
    if (n == 0) return;
    size_t start = (_send_rr++) % n;
    for (size_t i = 0; i < n; ++i) {
        auto it = _streams.find(ids[i]);
        if (it == _streams.end()) continue;
        StreamState& st = it->second;
        update_quantum(st);
        int64_t d = static_cast<int64_t>(st.sched_deficit) + static_cast<int64_t>(st.sched_quantum);
        if (d > INT32_MAX) d = INT32_MAX;
        st.sched_deficit = static_cast<int32_t>(d);
    }
    bool progress = true;
    int rounds = 0;
    while (progress && rounds < 2) {
        progress = false;
        for (size_t i = 0; i < n; ++i) {
            uint32_t sid = ids[(start + i) % n];
            uint32_t sent = try_send_data(sid);
            if (sent) progress = true;
        }
        rounds++;
    }
}

void http2_process::update_quantum(StreamState& st) {
    uint32_t base = _peer_max_frame_size ? _peer_max_frame_size : 16384u;
    uint32_t w = st.weight ? st.weight : 16u;
    uint64_t q = static_cast<uint64_t>(base) * static_cast<uint64_t>(w) / 16u;
    if (q < 1024u) q = 1024u;
    if (q > (1u << 30)) q = (1u << 30);
    st.sched_quantum = static_cast<uint32_t>(q);
}

void http2_process::enc_dyn_evict() {
    while (_enc_tbl_size > _enc_tbl_max && !_enc_dyn.empty()) {
        DynHdr& last = _enc_dyn.back();
        if (_enc_tbl_size >= last.sz) _enc_tbl_size -= last.sz; else _enc_tbl_size = 0;
        _enc_dyn.pop_back();
    }
}

void http2_process::enc_dyn_add(const std::string& name, const std::string& value) {
    size_t sz = 32u + name.size() + value.size();
    if (sz > _enc_tbl_max) {
        _enc_dyn.clear(); _enc_tbl_size = 0; return;
    }
    while (_enc_tbl_size + sz > _enc_tbl_max && !_enc_dyn.empty()) {
        DynHdr& last = _enc_dyn.back();
        if (_enc_tbl_size >= last.sz) _enc_tbl_size -= last.sz; else _enc_tbl_size = 0;
        _enc_dyn.pop_back();
    }
    DynHdr e;
    e.name = name;
    e.value = value;
    e.sz = sz;
    _enc_dyn.insert(_enc_dyn.begin(), std::move(e));
    _enc_tbl_size += sz;
}

uint32_t http2_process::enc_dyn_find_pair(const std::string& name, const std::string& value) const {
    for (size_t i = 0; i < _enc_dyn.size(); ++i) {
        if (_enc_dyn[i].name == name && _enc_dyn[i].value == value) {
            return static_cast<uint32_t>(i + 1);
        }
    }
    return 0;
}
