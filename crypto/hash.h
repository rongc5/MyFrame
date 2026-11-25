#pragma once

#include <cstddef>
#include <string>

namespace myframe {
namespace crypto {

enum class HashAlg {
    MD5,
    SHA1,
    SHA256,
    SHA512
};

// Streaming digest helper (RAII over OpenSSL EVP_MD_CTX). Non-copyable.
class Hasher {
public:
    explicit Hasher(HashAlg alg);
    ~Hasher();

    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    bool ok() const { return _valid; }
    bool reset();
    bool update(const void* data, std::size_t len);
    bool update(const std::string& data) { return update(data.data(), data.size()); }
    bool final_raw(std::string& out, std::string* err = nullptr);
    bool final_hex(std::string& out, std::string* err = nullptr);

private:
    HashAlg _alg;
    void* _ctx; // EVP_MD_CTX*
    bool _valid;
    bool _finalized;
};

// One-shot digest helpers.
bool hash_raw(HashAlg alg, const void* data, std::size_t len, std::string& out, std::string* err = nullptr);
bool hash_hex(HashAlg alg, const void* data, std::size_t len, std::string& out, std::string* err = nullptr);
bool hash_file_raw(HashAlg alg, const std::string& path, std::string& out, std::string* err = nullptr);
bool hash_file_hex(HashAlg alg, const std::string& path, std::string& out, std::string* err = nullptr);

std::string sha256_raw(const void* data, std::size_t len);
std::string sha256_hex(const void* data, std::size_t len);
inline std::string sha256_raw(const std::string& data) { return sha256_raw(data.data(), data.size()); }
inline std::string sha256_hex(const std::string& data) { return sha256_hex(data.data(), data.size()); }

std::string sha512_raw(const void* data, std::size_t len);
std::string sha512_hex(const void* data, std::size_t len);
inline std::string sha512_raw(const std::string& data) { return sha512_raw(data.data(), data.size()); }
inline std::string sha512_hex(const std::string& data) { return sha512_hex(data.data(), data.size()); }

std::string sha1_hex(const void* data, std::size_t len);
inline std::string sha1_hex(const std::string& data) { return sha1_hex(data.data(), data.size()); }

std::string md5_hex(const void* data, std::size_t len);
inline std::string md5_hex(const std::string& data) { return md5_hex(data.data(), data.size()); }

// HMAC helpers (constant-time verification built in).
bool hmac_raw(HashAlg alg, const void* key, std::size_t key_len,
              const void* data, std::size_t data_len,
              std::string& out, std::string* err = nullptr);
inline bool hmac_raw(HashAlg alg, const std::string& key, const std::string& data,
                     std::string& out, std::string* err = nullptr) {
    return hmac_raw(alg, key.data(), key.size(), data.data(), data.size(), out, err);
}

std::string hmac_sha256_raw(const std::string& key, const std::string& data);
std::string hmac_sha256_hex(const std::string& key, const std::string& data);
std::string hmac_sha512_raw(const std::string& key, const std::string& data);
std::string hmac_sha512_hex(const std::string& key, const std::string& data);

bool hmac_sha256_verify_raw(const std::string& key, const std::string& data, const std::string& expected_raw);
bool hmac_sha256_verify_hex(const std::string& key, const std::string& data, const std::string& expected_hex);
bool hmac_sha512_verify_raw(const std::string& key, const std::string& data, const std::string& expected_raw);
bool hmac_sha512_verify_hex(const std::string& key, const std::string& data, const std::string& expected_hex);

} // namespace crypto
} // namespace myframe
