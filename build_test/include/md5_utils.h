#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace myframe {

// Convenience constant for MD5 digest length in bytes.
constexpr std::size_t kMd5DigestLength = 16;

// Compute the MD5 digest for arbitrary data.
bool md5_digest(const void* data, std::size_t length, unsigned char* digest, unsigned int& digest_len);

// Overloads that avoid manual pointer/size plumbing for common containers.
bool md5_digest(const std::string& data, unsigned char* digest, unsigned int& digest_len);
bool md5_digest(const std::vector<std::uint8_t>& data, unsigned char* digest, unsigned int& digest_len);

// Hex helpers for raw buffers and common containers.
std::string md5_hex(const void* data, std::size_t length);
inline std::string md5_hex(const std::string& data) {
    return md5_hex(data.data(), data.size());
}
inline std::string md5_hex(const std::vector<std::uint8_t>& data) {
    return md5_hex(data.data(), data.size());
}

// File-based helpers (stream the file so large assets do not need to fit in memory).
bool md5_file_digest(const std::string& path, unsigned char* digest, unsigned int& digest_len);
std::string md5_file_hex(const std::string& path);

} // namespace myframe

