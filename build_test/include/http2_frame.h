#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace h2
{
// HTTP/2 constants
static constexpr const char* CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"; // 24 bytes
static constexpr size_t CONNECTION_PREFACE_LEN = 24;

enum FrameType : uint8_t {
    DATA          = 0x0,
    HEADERS       = 0x1,
    PRIORITY      = 0x2,
    RST_STREAM    = 0x3,
    SETTINGS      = 0x4,
    PUSH_PROMISE  = 0x5,
    PING          = 0x6,
    GOAWAY        = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION  = 0x9
};

enum Flags : uint8_t {
    FLAGS_ACK = 0x1
};

enum SettingId : uint16_t {
    SETTINGS_HEADER_TABLE_SIZE      = 0x1,
    SETTINGS_ENABLE_PUSH            = 0x2,
    SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    SETTINGS_INITIAL_WINDOW_SIZE    = 0x4,
    SETTINGS_MAX_FRAME_SIZE         = 0x5,
    SETTINGS_MAX_HEADER_LIST_SIZE   = 0x6
};

enum ErrorCode : uint32_t {
    NO_ERROR            = 0x0,
    PROTOCOL_ERROR      = 0x1,
    INTERNAL_ERROR      = 0x2,
    FLOW_CONTROL_ERROR  = 0x3,
    SETTINGS_TIMEOUT    = 0x4,
    STREAM_CLOSED       = 0x5,
    FRAME_SIZE_ERROR    = 0x6,
    REFUSED_STREAM      = 0x7,
    CANCEL              = 0x8
};

inline void write24(std::string& out, uint32_t v) {
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

inline void write32(std::string& out, uint32_t v) {
    out.push_back((char)((v >> 24) & 0xff));
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)(v & 0xff));
}

inline uint32_t read24(const unsigned char* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

inline uint32_t read32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

inline std::string make_frame_header(uint32_t len, FrameType type, uint8_t flags, uint32_t stream_id) {
    std::string out;
    out.reserve(9);
    write24(out, len & 0x00ffffffu);
    out.push_back((char)type);
    out.push_back((char)flags);
    write32(out, stream_id & 0x7fffffffu);
    return out;
}

inline std::string make_window_update(uint32_t stream_id, uint32_t increment) {
    std::string payload; payload.reserve(4);
    write32(payload, increment & 0x7fffffffu);
    std::string out = make_frame_header(4, WINDOW_UPDATE, 0, stream_id);
    out += payload; return out;
}

inline std::string make_rst_stream(uint32_t stream_id, ErrorCode ec) {
    std::string payload; write32(payload, (uint32_t)ec);
    std::string out = make_frame_header(4, RST_STREAM, 0, stream_id);
    out += payload; return out;
}

inline std::string make_settings_frame(bool ack = false) {
    std::string payload;
    if (!ack) {
        // SETTINGS_ENABLE_PUSH = 0
        payload.push_back((char)0x00);
        payload.push_back((char)SETTINGS_ENABLE_PUSH);
        payload.push_back((char)0x00);
        payload.push_back((char)0x00);
        payload.push_back((char)0x00);
        payload.push_back((char)0x00);
    }
    std::string out = make_frame_header((uint32_t)payload.size(), SETTINGS, ack ? FLAGS_ACK : 0, 0);
    out += payload;
    return out;
}

inline std::string make_settings_ack() { return make_settings_frame(true); }

inline std::string make_goaway(uint32_t last_stream_id, ErrorCode ec, const std::string& debug = std::string()) {
    std::string payload;
    write32(payload, last_stream_id & 0x7fffffffu);
    write32(payload, (uint32_t)ec);
    payload += debug;
    std::string out = make_frame_header((uint32_t)payload.size(), GOAWAY, 0, 0);
    out += payload;
    return out;
}

} // namespace h2
