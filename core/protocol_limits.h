// Process-wide protocol safety limits (HTTP request body size, reassembled
// WebSocket message size) plus strict numeric parsing helpers shared by the
// HTTP/1.x parsers.
//
// Each limit has a compiled-in default, can be overridden once through an
// environment variable (read on first use) and at runtime through a setter.
// Passing 0 to a setter restores the default (environment value, if any,
// otherwise the compiled-in constant).
#ifndef __MYFRAME_PROTOCOL_LIMITS_H__
#define __MYFRAME_PROTOCOL_LIMITS_H__

#include <cstddef>
#include <cstdint>
#include <string>

namespace myframe {

// Default maximum HTTP request body (Content-Length or decoded chunked) size:
// 16 MiB. Environment override: MYFRAME_MAX_HTTP_BODY_BYTES.
const size_t kDefaultMaxHttpBodyBytes = 16u * 1024u * 1024u;

// Default maximum reassembled WebSocket message size: 16 MiB.
// Environment override: MYFRAME_MAX_WS_MESSAGE_BYTES.
const size_t kDefaultMaxWsMessageBytes = 16u * 1024u * 1024u;

// Maximum number of header fields accepted in one HTTP/1.x header block.
const size_t MAX_HTTP_HEADER_COUNT = 256u;

size_t max_http_body_bytes();
void set_max_http_body_bytes(size_t bytes);

size_t max_ws_message_bytes();
void set_max_ws_message_bytes(size_t bytes);

// Strict decimal parser for Content-Length style values: the input must be a
// non-empty run of ASCII digits 0-9 only (no sign, whitespace, hex prefix or
// trailing characters) that fits into uint64_t. Returns false otherwise.
bool parse_strict_decimal_u64(const std::string& s, uint64_t& out);

// Strict hexadecimal parser for chunk-size values: 1..16 ASCII hex digits, no
// "0x" prefix, no sign, no whitespace. Returns false otherwise.
bool parse_strict_hex_u64(const std::string& s, uint64_t& out);

// True when c is an RFC 9110 "tchar" (valid header field-name character).
bool is_http_token_char(unsigned char c);

} // namespace myframe

#endif // __MYFRAME_PROTOCOL_LIMITS_H__
