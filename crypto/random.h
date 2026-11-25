#pragma once

#include <cstddef>
#include <string>

namespace myframe {
namespace crypto {

// Fill buf[0..len) with cryptographically secure random bytes (OpenSSL RAND_bytes).
bool fill_random(void* buf, std::size_t len, std::string* err = nullptr);

// Generate len cryptographically secure random bytes into out.
bool random_bytes(std::size_t len, std::string& out, std::string* err = nullptr);

// Convenience helpers returning random bytes encoded as lowercase hex or unpadded base64url.
// Returns empty string on CSPRNG failure.
std::string random_hex(std::size_t num_bytes);
std::string random_base64url(std::size_t num_bytes);

// Constant-time byte comparison (CRYPTO_memcmp) to prevent timing side channels.
bool secure_equal(const void* a, std::size_t a_len, const void* b, std::size_t b_len);
bool secure_equal(const std::string& a, const std::string& b);

// Overwrite sensitive memory via OPENSSL_cleanse so the compiler cannot optimize it away.
void secure_zero(void* buf, std::size_t len);
void secure_zero(std::string& s);

} // namespace crypto
} // namespace myframe
