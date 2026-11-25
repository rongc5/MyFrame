#pragma once

#include <cstddef>
#include <string>

namespace myframe {
namespace crypto {

// Hex encoding / strict decoding.
std::string hex_encode(const void* data, std::size_t len, bool uppercase = false);
inline std::string hex_encode(const std::string& data, bool uppercase = false) {
    return hex_encode(data.data(), data.size(), uppercase);
}
bool hex_decode(const std::string& hex, std::string& out, std::string* err = nullptr);

// Standard Base64 (RFC 4648 Section 4) encoding / strict decoding.
std::string base64_encode(const void* data, std::size_t len);
inline std::string base64_encode(const std::string& data) {
    return base64_encode(data.data(), data.size());
}
bool base64_decode(const std::string& b64, std::string& out, std::string* err = nullptr);

// URL-safe Base64 (RFC 4648 Section 5) encoding / strict decoding (accepts padded or unpadded).
std::string base64url_encode(const void* data, std::size_t len, bool with_padding = false);
inline std::string base64url_encode(const std::string& data, bool with_padding = false) {
    return base64url_encode(data.data(), data.size(), with_padding);
}
bool base64url_decode(const std::string& b64url, std::string& out, std::string* err = nullptr);

} // namespace crypto
} // namespace myframe
