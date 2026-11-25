#include "protocol_limits.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace myframe {
namespace {

size_t read_env_size(const char* name, size_t fallback) {
    const char* e = std::getenv(name);
    if (!e || !*e) return fallback;
    uint64_t v = 0;
    if (!parse_strict_decimal_u64(std::string(e), v) || v == 0) return fallback;
    if (v > static_cast<uint64_t>(static_cast<size_t>(-1))) return fallback;
    return static_cast<size_t>(v);
}

size_t env_default_http_body() {
    static std::once_flag once;
    static size_t value = kDefaultMaxHttpBodyBytes;
    std::call_once(once, []() {
        value = read_env_size("MYFRAME_MAX_HTTP_BODY_BYTES", kDefaultMaxHttpBodyBytes);
    });
    return value;
}

size_t env_default_ws_message() {
    static std::once_flag once;
    static size_t value = kDefaultMaxWsMessageBytes;
    std::call_once(once, []() {
        value = read_env_size("MYFRAME_MAX_WS_MESSAGE_BYTES", kDefaultMaxWsMessageBytes);
    });
    return value;
}

std::atomic<size_t> g_http_body_override(0);
std::atomic<size_t> g_ws_message_override(0);

int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

size_t max_http_body_bytes() {
    size_t v = g_http_body_override.load(std::memory_order_relaxed);
    return v ? v : env_default_http_body();
}

void set_max_http_body_bytes(size_t bytes) {
    g_http_body_override.store(bytes, std::memory_order_relaxed);
}

size_t max_ws_message_bytes() {
    size_t v = g_ws_message_override.load(std::memory_order_relaxed);
    return v ? v : env_default_ws_message();
}

void set_max_ws_message_bytes(size_t bytes) {
    g_ws_message_override.store(bytes, std::memory_order_relaxed);
}

bool parse_strict_decimal_u64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    const uint64_t kMax = static_cast<uint64_t>(-1);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (kMax - d) / 10u) return false;  // overflow
        v = v * 10u + d;
    }
    out = v;
    return true;
}

bool parse_strict_hex_u64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 16u) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const int d = hex_value(static_cast<unsigned char>(s[i]));
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint64_t>(d);  // at most 16 digits: no overflow
    }
    out = v;
    return true;
}

bool is_http_token_char(unsigned char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return std::strchr("!#$%&'*+-.^_`|~", static_cast<int>(c)) != NULL && c != 0;
}

} // namespace myframe
