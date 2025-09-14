#include "http2_client_process.h"
#include "http2_frame.h"
#include "base_thread.h"
#include <iostream>
#include <cstring>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace h2;

http2_client_process::http2_client_process(std::shared_ptr<base_net_obj> c,
                                           const std::string& method,
                                           const std::string& scheme,
                                           const std::string& host,
                                           const std::string& path,
                                           const std::map<std::string,std::string>& headers,
                                           const std::string& body)
    : base_data_process(c), _method(method), _scheme(scheme), _host(host), _path(path), _body(body), _headers(headers) {
    // load env knobs
    if (const char* e = ::getenv("MYFRAME_H2_WINUPDATE")) { long v = atol(e); if (v > 0) _winupdate_threshold = (uint32_t)v; }
    if (const char* e = ::getenv("MYFRAME_H2_PING_MS")) { long v = atol(e); if (v > 0) _ping_interval_ms = (uint32_t)v; }
    if (const char* e = ::getenv("MYFRAME_H2_TIMEOUT_MS")) { long v = atol(e); if (v > 0) _total_timeout_ms = (uint32_t)v; }
    if (const char* e = ::getenv("MYFRAME_H2_TRACE")) { _trace = (strcmp(e, "0") != 0 && strcasecmp(e, "false") != 0); }
    // setup timers based on knobs
    _start_ms = GetMilliSecond();
    std::shared_ptr<timer_msg> tp(new timer_msg); tp->_obj_id = 0; tp->_timer_type = H2_PING_TIMER_TYPE; tp->_time_length = _ping_interval_ms; add_timer(tp); _ping_scheduled = true;
    std::shared_ptr<timer_msg> tt(new timer_msg); tt->_obj_id = 0; tt->_timer_type = H2_TOTAL_TIMEOUT_TIMER_TYPE; tt->_time_length = _total_timeout_ms; add_timer(tt); _timeout_scheduled = true;
}

static void hpack_write_str(std::string& out, const std::string& s) {
    // No Huffman; 1-bit huffman flag=0 + 7-bit length (with multi-octet integer if needed)
    if (s.size() < 127) {
        out.push_back((char)s.size());
    } else {
        out.push_back((char)0x7f);
        uint32_t remain = (uint32_t)s.size() - 127;
        while (remain >= 128) { out.push_back((char)((remain % 128) + 128)); remain /= 128; }
        out.push_back((char)remain);
    }
    out.append(s);
}

static void hpack_literal_kv(std::string& out, const std::string& name, const std::string& value) {
    // Literal without indexing: 0000xxxx with name literal (xxxx=0)
    out.push_back((char)0x00);
    hpack_write_str(out, name);
    hpack_write_str(out, value);
}

// Literal without indexing, indexed name (HPACK static table index)
// Encoding: first byte 0000 with 4-bit prefix integer for index
static void hpack_literal_indexed_name(std::string& out, uint32_t index, const std::string& value) {
    // Write first byte with 4-bit prefix
    uint8_t first = 0x00; // 0000
    uint8_t max = 0x0f;   // 4-bit prefix max
    if (index < max) {
        out.push_back((char)((first & 0xF0) | (index & 0x0F)));
    } else {
        out.push_back((char)((first & 0xF0) | max));
        uint32_t v = index - max;
        while (v >= 128) { out.push_back((char)((v % 128) + 128)); v /= 128; }
        out.push_back((char)v);
    }
    hpack_write_str(out, value);
}

std::string http2_client_process::build_headers_block() const {
    std::string blk;
    // Pseudo headers must precede regular ones (common order). Use indexed names when available.
    const std::string& method = _method.empty()? std::string("GET") : _method;
    const std::string& scheme = _scheme.empty()? std::string("https") : _scheme;
    // :method (2 for GET, 3 for POST). For other methods, send literal name.
    if (method == "GET") hpack_literal_indexed_name(blk, 2, "GET");
    else if (method == "POST") hpack_literal_indexed_name(blk, 3, "POST");
    else hpack_literal_kv(blk, ":method", method);
    // :scheme (6 http, 7 https)
    if (scheme == "https") hpack_literal_indexed_name(blk, 7, "https");
    else if (scheme == "http") hpack_literal_indexed_name(blk, 6, "http");
    else hpack_literal_kv(blk, ":scheme", scheme);
    // :authority (1)
    hpack_literal_indexed_name(blk, 1, _host);
    // :path (4 is "/" name only; we still provide value)
    hpack_literal_indexed_name(blk, 4, _path.empty()? std::string("/") : _path);
    // Default headers if not provided
    auto has_ci = [&](const char* key){
        for (auto& kv : _headers) {
            if (kv.first.size() == std::strlen(key) && strncasecmp(kv.first.c_str(), key, kv.first.size()) == 0) return true;
        }
        return false;
    };
    if (!has_ci("user-agent")) hpack_literal_kv(blk, "user-agent", "myframe-h2-client");
    const char* no_ae = ::getenv("MYFRAME_NO_DEFAULT_AE");
    if (!has_ci("accept-encoding") && !(no_ae && (strcmp(no_ae, "1")==0 || strcasecmp(no_ae, "true")==0))) {
        hpack_literal_kv(blk, "accept-encoding", "gzip");
    }

    for (auto& kv : _headers) {
        // Lower-case header names per h2 requirement
        std::string name = kv.first; for (auto& c : name) c = (char)tolower(c);
        hpack_literal_kv(blk, name, kv.second);
    }
    return blk;
}

void http2_client_process::enqueue_preface_and_request() {
    if (_enqueued) return; _enqueued = true;
    // 1) Connection preface
    put_send_buf(new std::string(std::string(CONNECTION_PREFACE, CONNECTION_PREFACE_LEN)));
    // 2) SETTINGS (ENABLE_PUSH=0)
    put_send_buf(new std::string(make_settings_frame(false)));
    // 3) HEADERS (stream 1)
    std::string blk = build_headers_block();
    uint8_t flags = 0x4 /*END_HEADERS*/ | (_body.empty()? 0x1 /*END_STREAM*/ : 0x0);
    std::string hdr = make_frame_header((uint32_t)blk.size(), HEADERS, flags, 1);
    if (_trace) {
        std::cout << "[h2] send preface+SETTINGS+HEADERS stream=1 flags=0x" << std::hex << (unsigned)flags << std::dec << std::endl;
        std::cout << "[h2] headers block len=" << blk.size() << " hex=";
        size_t dump = blk.size() < 48 ? blk.size() : 48;
        for (size_t i=0;i<dump;++i) {
            unsigned int b = (unsigned char)blk[i];
            char hi[] = {"0123456789abcdef"[(b>>4)&0xF], "0123456789abcdef"[b&0xF], 0};
            std::cout << hi;
        }
        std::cout << (blk.size()>48?"...":"") << std::endl;
    }
    put_send_buf(new std::string(hdr + blk));
    // 4) Optional DATA (single frame)
    if (!_body.empty()) {
        std::string data_hdr = make_frame_header((uint32_t)_body.size(), DATA, 0x1 /*END_STREAM*/, 1);
        put_send_buf(new std::string(data_hdr + _body));
    }
    _sent_all = true;
}

std::string* http2_client_process::get_send_buf() {
    if (!_sent_all) enqueue_preface_and_request();
    return base_data_process::get_send_buf();
}

static uint32_t read24u(const unsigned char* p){ return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint32_t read32u(const unsigned char* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

// Minimal HPACK decode helpers (no Huffman) to extract :status if present
static bool hp_read_int(const unsigned char*& p, const unsigned char* e, int prefix_bits, uint32_t& out){ if (p>=e) return false; uint8_t first=*p++; uint32_t max=(1u<<prefix_bits)-1u; uint32_t val=(first & max); if (val<max){ out=val; return true;} uint32_t m=0,add=0; do{ if(p>=e) return false; add=(uint32_t)(*p & 0x7f); val += (add<<m); m+=7; }while((*p++ & 0x80)==0x80); out=val; return true; }
static bool hp_read_str(const unsigned char*& p, const unsigned char* e, std::string& s){ if (p>=e) return false; uint8_t b=*p++; bool h=(b&0x80)!=0; uint32_t len=(b&0x7f); if (len==0x7f){ uint32_t m=0,add=0; len=0x7f; do{ if(p>=e) return false; add=(uint32_t)(*p & 0x7f); len += (add<<m); m+=7; }while((*p++ & 0x80)==0x80);} if ((size_t)(e-p) < len) return false; if (h) return false; s.assign((const char*)p, (const char*)p+len); p += len; return true; }

bool http2_client_process::handle_headers_payload(const unsigned char* p, uint32_t len, uint8_t flags) {
    uint8_t pad_len = 0; const unsigned char* pp = p; const unsigned char* pe = p + len;
    if (flags & 0x08 /*PADDED*/) { if (pp>=pe) return false; pad_len = *pp++; }
    if (flags & 0x20 /*PRIORITY*/) { if ((size_t)(pe-pp) < 5) return false; pp += 5; }
    size_t header_len = (size_t)(pe-pp); if (header_len < pad_len) return false; size_t remain = header_len - pad_len;
    std::string block; block.assign((const char*)pp, remain);
    // If not END_HEADERS, expect CONTINUATION frames in parse_frames()
    // Minimal decode: only extract :status
    const unsigned char* bp = (const unsigned char*)block.data(); const unsigned char* be = bp + block.size();
    while (bp < be) {
        if (*bp & 0x80) { uint32_t idx=0; if (!hp_read_int(bp,be,7,idx)) return false; if (idx>=8 && idx<=14) { static const int map[]={0,0,0,0,0,0,0,200,204,206,304,400,404,500}; int v = map[idx]; if (v) _status = v; } }
        else if ((*bp & 0x40) == 0x40) { uint32_t n=0; std::string name,value; uint8_t first=*bp; if ((first & 0x3f)==0){ ++bp; if (!hp_read_str(bp,be,name)) return false; } else { if (!hp_read_int(bp,be,6,n)) return false; if (n>=8 && n<=14) name=":status"; }
            if (!hp_read_str(bp,be,value)) return false; if (name==":status") _status = atoi(value.c_str()); }
        else { uint32_t n=0; std::string name,value; uint8_t first=*bp; if ((first & 0x0f)==0){ ++bp; if (!hp_read_str(bp,be,name)) return false; } else { if (!hp_read_int(bp,be,4,n)) return false; if (n==0) return false; if (n>=8 && n<=14) name=":status"; }
            if (!hp_read_str(bp,be,value)) return false; if (name==":status") _status = atoi(value.c_str()); }
    }
    return true;
}

bool http2_client_process::parse_frames() {
    size_t n = _in.size(); size_t off = 0; while (n - off >= 9) {
        const unsigned char* p = &_in[off]; uint32_t len = read24u(p); uint8_t type = p[3]; uint8_t flags = p[4]; uint32_t sid = read32u(p+5) & 0x7fffffffu; if (n - off < 9 + len) break;
        if (_trace) {
            std::cout << "[h2] frame: len=" << len << " type=0x" << std::hex << (unsigned)type << std::dec << " flags=0x" << std::hex << (unsigned)flags << std::dec << " sid=" << sid << std::endl;
        }
        const unsigned char* payload = p + 9;
        if (type == SETTINGS) { if ((flags & FLAGS_ACK) == 0) { put_send_buf(new std::string(make_settings_ack())); } }
        else if (type == HEADERS && sid == 1) {
            PDEBUG("[h2] HEADERS len=%u flags=0x%x sid=%u", len, flags, sid);
            if (!handle_headers_payload(payload, len, flags)) return false;
            if ((flags & 0x4) == 0) {
                // need to gather CONTINUATIONs until END_HEADERS
                size_t cont_off = off + 9 + len; while (n - cont_off >= 9) {
                    const unsigned char* ch = &_in[cont_off]; uint32_t clen = read24u(ch); uint8_t ctype = ch[3]; uint8_t cflags = ch[4]; uint32_t csid = read32u(ch+5) & 0x7fffffffu; if (n - cont_off < 9 + clen) break; if (ctype != CONTINUATION || csid != 1) break; // ignore unexpected
                    if (clen) { if (!handle_headers_payload(ch+9, clen, cflags)) return false; }
                    cont_off += 9 + clen; if (cflags & 0x4) break; }
            }
            if (flags & 0x1 /*END_STREAM*/) {
                _headers_end_stream = true;
                // Only treat as done for statuses without body or HEAD method
                std::string m = _method; for (auto& c : m) c = (char)tolower((unsigned char)c);
                if (_status == 204 || _status == 304 || m == "head") {
                    _response_done = true;
                } else {
                    PDEBUG("[h2] HEADERS had END_STREAM but status=%d method=%s -> waiting for DATA", _status, m.c_str());
                }
            }
        }
        else if (type == DATA && sid == 1) {
            PDEBUG("[h2] DATA len=%u flags=0x%x sid=%u", len, flags, sid);
            // handle padding
            const unsigned char* pp = payload; uint32_t remain = len; if (flags & 0x08) { if (remain<1) return false; uint8_t pad=*pp++; remain-=1; if (pad>remain) return false; remain -= pad; }
            if (remain) {
                _resp_body.append((const char*)pp, remain);
                PDEBUG("[h2] DATA appended %u bytes, total=%zu", remain, _resp_body.size());
                // Accumulate credits and send WINDOW_UPDATE in batches
                _conn_win_credits += remain;
                _strm1_win_credits += remain;
                if (_conn_win_credits >= _winupdate_threshold) { put_send_buf(new std::string(make_window_update(0, _conn_win_credits))); _conn_win_credits = 0; }
                if (_strm1_win_credits >= _winupdate_threshold) { put_send_buf(new std::string(make_window_update(1, _strm1_win_credits))); _strm1_win_credits = 0; }
            }
            if (flags & 0x1) { _response_done = true; }
        }
        else if (type == PING) {
            if ((flags & FLAGS_ACK) == 0) {
                // echo with ACK
                std::string ack = make_frame_header(8, PING, FLAGS_ACK, 0);
                ack.append((const char*)payload, 8);
                put_send_buf(new std::string(ack));
            }
        }
        else if (type == GOAWAY) {
            if (len >= 8) {
                uint32_t last_sid = read32u(payload) & 0x7fffffffu;
                uint32_t err = read32u(payload+4);
                std::cout << "[h2] GOAWAY last_stream_id=" << last_sid << " error=0x" << std::hex << err << std::dec << std::endl;
            } else {
                std::cout << "[h2] GOAWAY len=" << len << std::endl;
            }
            _response_done = true;
        }
        off += 9 + len;
    }
    if (off) { _in.erase(_in.begin(), _in.begin()+off); }
    return true;
}

size_t http2_client_process::process_recv_buf(const char* buf, size_t len) {
    if (len) {
        if (_trace) {
            std::cout << "[h2] recv bytes: " << len << std::endl;
            size_t dump = len < 32 ? len : 32;
            std::cout << "[h2] recv hex: ";
            for (size_t i=0;i<dump;++i) {
                unsigned int b = (unsigned char)buf[i];
                char hi[] = {"0123456789abcdef"[(b>>4)&0xF], "0123456789abcdef"[b&0xF], 0};
                std::cout << hi;
            }
            std::cout << (len>32?"...":"") << std::endl;
        }
        _in.insert(_in.end(), (const unsigned char*)buf, (const unsigned char*)buf + len);
    }
    if (!parse_frames()) return 0;
    if (_response_done) {
        if (_status == 0) _status = 200;
        std::cout << "H2 Response Status: " << _status << std::endl;
        std::cout << "Response Body Length: " << _resp_body.size() << std::endl;
#ifdef HAVE_ZLIB
        // Auto-decompress gzip if detected (magic 0x1f8b)
        bool gz = (_resp_body.size() >= 2 && (uint8_t)_resp_body[0] == 0x1f && (uint8_t)_resp_body[1] == 0x8b);
        if (gz) {
            std::string out; out.resize(64 * 1024);
            z_stream strm; memset(&strm, 0, sizeof(strm));
            int ret = inflateInit2(&strm, 16 + MAX_WBITS);
            if (ret == Z_OK) {
                strm.next_in = (Bytef*)_resp_body.data();
                strm.avail_in = (uInt)_resp_body.size();
                int zret = Z_OK; std::string dec;
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
                } else {
                    std::cout << "[h2] gzip decompress failed, zret=" << zret << std::endl;
                    if (_resp_body.size() < 1000) std::cout << "Response Body (raw): " << _resp_body << std::endl;
                }
            } else {
                if (_resp_body.size() < 1000) std::cout << "Response Body (raw): " << _resp_body << std::endl;
            }
        } else {
            if (_resp_body.size() < 1000) std::cout << "Response Body: " << _resp_body << std::endl;
        }
#else
        if (_resp_body.size() < 1000) std::cout << "Response Body: " << _resp_body << std::endl;
#endif
        // Do not stop all threads automatically; allow the owner to decide.
    }
    return len;
}

void http2_client_process::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (!t_msg) return;
    if (t_msg->_timer_type == H2_PING_TIMER_TYPE && !_response_done) {
        // send PING with opaque data
        std::string ping = make_frame_header(8, PING, 0, 0);
        const char opaque[8] = { 'm','y','f','r','a','m','e','1' };
        ping.append(opaque, 8);
        put_send_buf(new std::string(ping));
        // reschedule
        std::shared_ptr<timer_msg> tp(new timer_msg); tp->_obj_id = 0; tp->_timer_type = H2_PING_TIMER_TYPE; tp->_time_length = _ping_interval_ms; add_timer(tp);
    } else if (t_msg->_timer_type == H2_TOTAL_TIMEOUT_TIMER_TYPE && !_response_done) {
        std::cout << "H2 total timeout, requesting close" << std::endl;
        close_now();
    }
}
