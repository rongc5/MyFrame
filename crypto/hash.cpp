#include "hash.h"
#include "codec.h"
#include "random.h"

#include <climits>
#include <fstream>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace myframe {
namespace crypto {

namespace {

static const EVP_MD* to_evp_md(HashAlg alg) {
    switch (alg) {
        case HashAlg::MD5:    return EVP_md5();
        case HashAlg::SHA1:   return EVP_sha1();
        case HashAlg::SHA256: return EVP_sha256();
        case HashAlg::SHA512: return EVP_sha512();
        default:              return nullptr;
    }
}

static std::string last_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "openssl digest error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

static void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

} // namespace

Hasher::Hasher(HashAlg alg)
    : _alg(alg), _ctx(nullptr), _valid(false), _finalized(false) {
    const EVP_MD* md = to_evp_md(alg);
    if (!md) return;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx && EVP_DigestInit_ex(ctx, md, nullptr) == 1) {
        _ctx = ctx;
        _valid = true;
    } else if (ctx) {
        EVP_MD_CTX_free(ctx);
    }
}

Hasher::~Hasher() {
    if (_ctx) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(_ctx));
        _ctx = nullptr;
    }
}

bool Hasher::reset() {
    const EVP_MD* md = to_evp_md(_alg);
    if (!md) return false;
    if (!_ctx) {
        _ctx = EVP_MD_CTX_new();
        if (!_ctx) return false;
    }
    _valid = (EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(_ctx), md, nullptr) == 1);
    _finalized = false;
    return _valid;
}

bool Hasher::update(const void* data, std::size_t len) {
    if (!_valid || _finalized || !_ctx) return false;
    if (len == 0) return true;
    if (!data) return false;
    return EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(_ctx), data, len) == 1;
}

bool Hasher::final_raw(std::string& out, std::string* err) {
    if (!_valid || _finalized || !_ctx) {
        out.clear();
        set_err(err, "hasher is not in an active state");
        return false;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(_ctx), digest, &digest_len) != 1) {
        _valid = false;
        out.clear();
        set_err(err, "EVP_DigestFinal_ex failed: " + last_openssl_error());
        return false;
    }
    _finalized = true;
    std::string tmp(reinterpret_cast<const char*>(digest), digest_len);
    out.swap(tmp);
    return true;
}

bool Hasher::final_hex(std::string& out, std::string* err) {
    std::string raw;
    if (!final_raw(raw, err)) {
        out.clear();
        return false;
    }
    out = hex_encode(raw, false);
    return true;
}

bool hash_raw(HashAlg alg, const void* data, std::size_t len, std::string& out, std::string* err) {
    if (!to_evp_md(alg)) {
        out.clear();
        set_err(err, "failed to initialize digest context");
        return false;
    }
    if (len > 0 && !data) {
        out.clear();
        set_err(err, "digest data pointer is null with non-zero length");
        return false;
    }
    Hasher h(alg);
    if (!h.ok()) {
        out.clear();
        set_err(err, "failed to initialize digest context");
        return false;
    }
    if (len > 0 && !h.update(data, len)) {
        out.clear();
        set_err(err, "EVP_DigestUpdate failed");
        return false;
    }
    std::string tmp;
    if (!h.final_raw(tmp, err)) {
        out.clear();
        return false;
    }
    out.swap(tmp);
    return true;
}

bool hash_hex(HashAlg alg, const void* data, std::size_t len, std::string& out, std::string* err) {
    std::string raw;
    if (!hash_raw(alg, data, len, raw, err)) {
        out.clear();
        return false;
    }
    out = hex_encode(raw, false);
    return true;
}

bool hash_file_raw(HashAlg alg, const std::string& path, std::string& out, std::string* err) {
    Hasher h(alg);
    if (!h.ok()) {
        set_err(err, "failed to initialize digest context");
        out.clear();
        return false;
    }
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open()) {
        set_err(err, "failed to open file: " + path);
        out.clear();
        return false;
    }
    char buf[16384];
    while (ifs.good()) {
        ifs.read(buf, sizeof(buf));
        std::streamsize n = ifs.gcount();
        if (n > 0 && !h.update(buf, static_cast<std::size_t>(n))) {
            set_err(err, "digest update failed");
            out.clear();
            return false;
        }
    }
    if (ifs.bad() || (!ifs.eof() && ifs.fail())) {
        set_err(err, "failed reading file: " + path);
        out.clear();
        return false;
    }
    std::string tmp;
    if (!h.final_raw(tmp, err)) {
        out.clear();
        return false;
    }
    out.swap(tmp);
    return true;
}

bool hash_file_hex(HashAlg alg, const std::string& path, std::string& out, std::string* err) {
    std::string raw;
    if (!hash_file_raw(alg, path, raw, err)) {
        out.clear();
        return false;
    }
    out = hex_encode(raw, false);
    return true;
}

std::string sha256_raw(const void* data, std::size_t len) {
    std::string out;
    return hash_raw(HashAlg::SHA256, data, len, out) ? out : std::string();
}

std::string sha256_hex(const void* data, std::size_t len) {
    std::string out;
    return hash_hex(HashAlg::SHA256, data, len, out) ? out : std::string();
}

std::string sha512_raw(const void* data, std::size_t len) {
    std::string out;
    return hash_raw(HashAlg::SHA512, data, len, out) ? out : std::string();
}

std::string sha512_hex(const void* data, std::size_t len) {
    std::string out;
    return hash_hex(HashAlg::SHA512, data, len, out) ? out : std::string();
}

std::string sha1_hex(const void* data, std::size_t len) {
    std::string out;
    return hash_hex(HashAlg::SHA1, data, len, out) ? out : std::string();
}

std::string md5_hex(const void* data, std::size_t len) {
    std::string out;
    return hash_hex(HashAlg::MD5, data, len, out) ? out : std::string();
}

bool hmac_raw(HashAlg alg, const void* key, std::size_t key_len,
              const void* data, std::size_t data_len,
              std::string& out, std::string* err) {
    const EVP_MD* md = to_evp_md(alg);
    if (!md) {
        out.clear();
        set_err(err, "unsupported HMAC hash algorithm");
        return false;
    }
    if (key_len > 0 && !key) {
        out.clear();
        set_err(err, "HMAC key pointer is null with non-zero length");
        return false;
    }
    if (data_len > 0 && !data) {
        out.clear();
        set_err(err, "HMAC data pointer is null with non-zero length");
        return false;
    }
    if (key_len > static_cast<std::size_t>(INT_MAX)) {
        out.clear();
        set_err(err, "HMAC key length exceeds INT_MAX");
        return false;
    }

    static const unsigned char kEmpty = 0;
    const void* kptr = (key_len > 0) ? key : static_cast<const void*>(&kEmpty);
    const unsigned char* dptr = (data_len > 0)
        ? static_cast<const unsigned char*>(data) : &kEmpty;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    if (!HMAC(md, kptr, static_cast<int>(key_len), dptr, data_len, mac, &mac_len)) {
        secure_zero(mac, sizeof(mac));
        out.clear();
        set_err(err, "HMAC failed: " + last_openssl_error());
        return false;
    }
    std::string tmp(reinterpret_cast<const char*>(mac), mac_len);
    secure_zero(mac, sizeof(mac));
    out.swap(tmp);
    secure_zero(tmp);
    return true;
}

std::string hmac_sha256_raw(const std::string& key, const std::string& data) {
    std::string out;
    return hmac_raw(HashAlg::SHA256, key, data, out) ? out : std::string();
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    std::string raw = hmac_sha256_raw(key, data);
    std::string hex = hex_encode(raw, false);
    secure_zero(raw);
    return hex;
}

std::string hmac_sha512_raw(const std::string& key, const std::string& data) {
    std::string out;
    return hmac_raw(HashAlg::SHA512, key, data, out) ? out : std::string();
}

std::string hmac_sha512_hex(const std::string& key, const std::string& data) {
    std::string raw = hmac_sha512_raw(key, data);
    std::string hex = hex_encode(raw, false);
    secure_zero(raw);
    return hex;
}

bool hmac_sha256_verify_raw(const std::string& key, const std::string& data,
                            const std::string& expected_raw) {
    std::string computed;
    if (!hmac_raw(HashAlg::SHA256, key, data, computed)) return false;
    bool ok = secure_equal(computed, expected_raw);
    secure_zero(computed);
    return ok;
}

bool hmac_sha256_verify_hex(const std::string& key, const std::string& data,
                            const std::string& expected_hex) {
    std::string expected_raw;
    if (!hex_decode(expected_hex, expected_raw)) return false;
    bool ok = hmac_sha256_verify_raw(key, data, expected_raw);
    secure_zero(expected_raw);
    return ok;
}

bool hmac_sha512_verify_raw(const std::string& key, const std::string& data,
                            const std::string& expected_raw) {
    std::string computed;
    if (!hmac_raw(HashAlg::SHA512, key, data, computed)) return false;
    bool ok = secure_equal(computed, expected_raw);
    secure_zero(computed);
    return ok;
}

bool hmac_sha512_verify_hex(const std::string& key, const std::string& data,
                            const std::string& expected_hex) {
    std::string expected_raw;
    if (!hex_decode(expected_hex, expected_raw)) return false;
    bool ok = hmac_sha512_verify_raw(key, data, expected_raw);
    secure_zero(expected_raw);
    return ok;
}

} // namespace crypto
} // namespace myframe
