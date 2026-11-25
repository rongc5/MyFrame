#include "random.h"
#include "codec.h"

#include <climits>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/rand.h>

namespace myframe {
namespace crypto {

namespace {

static std::string last_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "openssl operation failed";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

static void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

} // namespace

bool fill_random(void* buf, std::size_t len, std::string* err) {
    if (len == 0) return true;
    if (!buf) {
        set_err(err, "random output buffer is null");
        return false;
    }
    unsigned char* p = static_cast<unsigned char*>(buf);
    std::size_t remain = len;
    const std::size_t kChunk = 16u * 1024u * 1024u;
    while (remain > 0) {
        int n = static_cast<int>(remain < kChunk ? remain : kChunk);
        if (RAND_bytes(p, n) != 1) {
            set_err(err, "RAND_bytes failed: " + last_openssl_error());
            return false;
        }
        p += n;
        remain -= static_cast<std::size_t>(n);
    }
    return true;
}

bool random_bytes(std::size_t len, std::string& out, std::string* err) {
    std::string tmp(len, '\0');
    if (len > 0 && !fill_random(&tmp[0], len, err)) {
        out.clear();
        return false;
    }
    out.swap(tmp);
    return true;
}

std::string random_hex(std::size_t num_bytes) {
    std::string raw;
    if (!random_bytes(num_bytes, raw)) return std::string();
    std::string res = hex_encode(raw, false);
    secure_zero(raw);
    return res;
}

std::string random_base64url(std::size_t num_bytes) {
    std::string raw;
    if (!random_bytes(num_bytes, raw)) return std::string();
    std::string res = base64url_encode(raw, false);
    secure_zero(raw);
    return res;
}

bool secure_equal(const void* a, std::size_t a_len, const void* b, std::size_t b_len) {
    if (a_len != b_len) return false;
    if (a_len == 0) return true;
    if (!a || !b) return false;
    return CRYPTO_memcmp(a, b, a_len) == 0;
}

bool secure_equal(const std::string& a, const std::string& b) {
    return secure_equal(a.data(), a.size(), b.data(), b.size());
}

void secure_zero(void* buf, std::size_t len) {
    if (buf && len > 0) {
        OPENSSL_cleanse(buf, len);
    }
}

void secure_zero(std::string& s) {
    if (!s.empty()) {
        OPENSSL_cleanse(&s[0], s.size());
    }
    std::string().swap(s);
}

} // namespace crypto
} // namespace myframe
