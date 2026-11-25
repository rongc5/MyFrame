#pragma once

#include <cstddef>
#include <string>

namespace myframe {
namespace crypto {

// PBKDF2-HMAC-SHA256 (RFC 2898 / RFC 6070).
// iterations must be > 0 and out_len > 0.
bool pbkdf2_hmac_sha256(const std::string& password,
                        const std::string& salt,
                        int iterations,
                        std::size_t out_len,
                        std::string& out,
                        std::string* err = nullptr);

// HKDF-SHA256 (RFC 5869): Extract + Expand.
// salt and info may be empty; out_len must be in [1, 255 * 32].
bool hkdf_sha256(const std::string& ikm,
                 const std::string& salt,
                 const std::string& info,
                 std::size_t out_len,
                 std::string& out,
                 std::string* err = nullptr);

} // namespace crypto
} // namespace myframe
