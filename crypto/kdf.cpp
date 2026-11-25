#include "kdf.h"
#include "hash.h"
#include "random.h"

#include <climits>
#include <openssl/err.h>
#include <openssl/evp.h>

namespace myframe {
namespace crypto {

namespace {

static void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

static std::string last_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "openssl kdf error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

bool pbkdf2_hmac_sha256(const std::string& password,
                        const std::string& salt,
                        int iterations,
                        std::size_t out_len,
                        std::string& out,
                        std::string* err) {
    if (iterations <= 0) {
        out.clear();
        set_err(err, "PBKDF2 iterations must be > 0");
        return false;
    }
    if (out_len == 0 || out_len > static_cast<std::size_t>(INT_MAX)) {
        out.clear();
        set_err(err, "PBKDF2 output length out of range");
        return false;
    }
    if (password.size() > static_cast<std::size_t>(INT_MAX) ||
        salt.size() > static_cast<std::size_t>(INT_MAX)) {
        out.clear();
        set_err(err, "PBKDF2 input exceeds INT_MAX");
        return false;
    }
    static const unsigned char kEmpty = 0;
    const char* pass_ptr = password.empty() ? "" : password.data();
    const unsigned char* salt_ptr = salt.empty()
        ? &kEmpty : reinterpret_cast<const unsigned char*>(salt.data());

    std::string tmp(out_len, '\0');
    if (PKCS5_PBKDF2_HMAC(pass_ptr, static_cast<int>(password.size()),
                          salt_ptr, static_cast<int>(salt.size()),
                          iterations, EVP_sha256(),
                          static_cast<int>(out_len),
                          reinterpret_cast<unsigned char*>(&tmp[0])) != 1) {
        secure_zero(tmp);
        out.clear();
        set_err(err, "PKCS5_PBKDF2_HMAC failed: " + last_openssl_error());
        return false;
    }
    out.swap(tmp);
    return true;
}

bool hkdf_sha256(const std::string& ikm,
                 const std::string& salt,
                 const std::string& info,
                 std::size_t out_len,
                 std::string& out,
                 std::string* err) {
    static const std::size_t kHashLen = 32u;
    if (out_len == 0 || out_len > 255u * kHashLen) {
        out.clear();
        set_err(err, "HKDF-SHA256 output length must be in [1, 8160]");
        return false;
    }

    // RFC 5869 Section 2.2: HKDF-Extract(salt, IKM) -> PRK
    std::string effective_salt = salt.empty() ? std::string(kHashLen, '\0') : salt;
    std::string prk;
    if (!hmac_raw(HashAlg::SHA256, effective_salt, ikm, prk, err)) {
        out.clear();
        return false;
    }

    // RFC 5869 Section 2.3: HKDF-Expand(PRK, info, L) -> OKM
    const std::size_t blocks = (out_len + kHashLen - 1u) / kHashLen;
    std::string okm;
    okm.reserve(blocks * kHashLen);
    std::string prev_t;
    std::string msg;
    msg.reserve(kHashLen + info.size() + 1u);

    for (std::size_t i = 1; i <= blocks; ++i) {
        msg.clear();
        msg.append(prev_t);
        msg.append(info);
        msg.push_back(static_cast<char>(i));

        std::string cur_t;
        if (!hmac_raw(HashAlg::SHA256, prk, msg, cur_t, err)) {
            secure_zero(prk);
            secure_zero(prev_t);
            secure_zero(msg);
            secure_zero(okm);
            out.clear();
            return false;
        }
        okm.append(cur_t);
        secure_zero(prev_t);
        prev_t.swap(cur_t);
    }

    secure_zero(prk);
    secure_zero(prev_t);
    secure_zero(msg);

    okm.resize(out_len);
    out.swap(okm);
    return true;
}

} // namespace crypto
} // namespace myframe
