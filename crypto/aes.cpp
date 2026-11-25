#include "aes.h"
#include "codec.h"
#include "random.h"

#include <memory>
#include <openssl/err.h>
#include <openssl/evp.h>

namespace myframe {
namespace crypto {

namespace {

static const std::size_t kEvpChunkSize = 16u * 1024u * 1024u;

struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* p) const {
        if (p) EVP_CIPHER_CTX_free(p);
    }
};
typedef std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter> CipherCtxPtr;

static bool fail_out(std::string& out, std::string* err, const std::string& msg) {
    out.clear();
    if (err) *err = msg;
    return false;
}

static bool fail_out2(std::string& out1, std::string& out2, std::string* err, const std::string& msg) {
    out1.clear();
    out2.clear();
    if (err) *err = msg;
    return false;
}

static std::string last_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "openssl cipher error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

static const EVP_CIPHER* aes_gcm_cipher(std::size_t key_len) {
    switch (key_len) {
        case 16u: return EVP_aes_128_gcm();
        case 24u: return EVP_aes_192_gcm();
        case 32u: return EVP_aes_256_gcm();
        default:  return nullptr;
    }
}

static const EVP_CIPHER* aes_cbc_cipher(std::size_t key_len) {
    switch (key_len) {
        case 16u: return EVP_aes_128_cbc();
        case 24u: return EVP_aes_192_cbc();
        case 32u: return EVP_aes_256_cbc();
        default:  return nullptr;
    }
}

static const EVP_CIPHER* aes_ctr_cipher(std::size_t key_len) {
    switch (key_len) {
        case 16u: return EVP_aes_128_ctr();
        case 24u: return EVP_aes_192_ctr();
        case 32u: return EVP_aes_256_ctr();
        default:  return nullptr;
    }
}

static bool feed_aad(EVP_CIPHER_CTX* ctx, const std::string& aad, bool encrypt) {
    std::size_t off = 0;
    while (off < aad.size()) {
        int n = static_cast<int>((aad.size() - off) < kEvpChunkSize
                                     ? (aad.size() - off) : kEvpChunkSize);
        int outl = 0;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(aad.data() + off);
        int rc = encrypt ? EVP_EncryptUpdate(ctx, nullptr, &outl, p, n)
                         : EVP_DecryptUpdate(ctx, nullptr, &outl, p, n);
        if (rc != 1) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

bool aes_gcm_encrypt(const std::string& key,
                     const std::string& nonce,
                     const std::string& plaintext,
                     std::string& out_ciphertext,
                     std::string& out_tag,
                     const std::string& aad,
                     std::string* err) {
    const EVP_CIPHER* cipher = aes_gcm_cipher(key.size());
    if (!cipher) {
        return fail_out2(out_ciphertext, out_tag, err, "AES key length must be 16, 24, or 32 bytes");
    }
    if (nonce.empty() || nonce.size() > 1024u) {
        return fail_out2(out_ciphertext, out_tag, err, "AES-GCM nonce length is invalid");
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return fail_out2(out_ciphertext, out_tag, err, "EVP_CIPHER_CTX_new failed");
    }
    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
        return fail_out2(out_ciphertext, out_tag, err, "AES-GCM encrypt init failed: " + last_openssl_error());
    }

    if (!aad.empty() && !feed_aad(ctx.get(), aad, true)) {
        return fail_out2(out_ciphertext, out_tag, err, "AES-GCM AAD failed: " + last_openssl_error());
    }

    std::string ct(plaintext.size(), '\0');
    std::size_t total = 0;
    std::size_t off = 0;
    while (off < plaintext.size()) {
        int n = static_cast<int>((plaintext.size() - off) < kEvpChunkSize
                                     ? (plaintext.size() - off) : kEvpChunkSize);
        int outl = 0;
        if (EVP_EncryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(&ct[total]), &outl,
                              reinterpret_cast<const unsigned char*>(plaintext.data() + off),
                              n) != 1) {
            return fail_out2(out_ciphertext, out_tag, err, "AES-GCM encrypt update failed: " + last_openssl_error());
        }
        total += static_cast<std::size_t>(outl);
        off += static_cast<std::size_t>(n);
    }

    unsigned char final_buf[kAesBlockSize];
    int finl = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), final_buf, &finl) != 1) {
        return fail_out2(out_ciphertext, out_tag, err, "AES-GCM encrypt final failed: " + last_openssl_error());
    }
    if (finl > 0) {
        ct.resize(total);
        ct.append(reinterpret_cast<const char*>(final_buf), static_cast<std::size_t>(finl));
    } else {
        ct.resize(total);
    }

    std::string tag(kAesGcmTagSize, '\0');
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(kAesGcmTagSize), &tag[0]) != 1) {
        return fail_out2(out_ciphertext, out_tag, err, "AES-GCM get tag failed: " + last_openssl_error());
    }

    out_ciphertext.swap(ct);
    out_tag.swap(tag);
    return true;
}

bool aes_gcm_decrypt(const std::string& key,
                     const std::string& nonce,
                     const std::string& ciphertext,
                     const std::string& tag,
                     std::string& out_plaintext,
                     const std::string& aad,
                     std::string* err) {
    const EVP_CIPHER* cipher = aes_gcm_cipher(key.size());
    if (!cipher) {
        return fail_out(out_plaintext, err, "AES key length must be 16, 24, or 32 bytes");
    }
    if (nonce.empty() || nonce.size() > 1024u) {
        return fail_out(out_plaintext, err, "AES-GCM nonce length is invalid");
    }
    if (tag.size() != kAesGcmTagSize) {
        return fail_out(out_plaintext, err, "AES-GCM tag must be 16 bytes");
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return fail_out(out_plaintext, err, "EVP_CIPHER_CTX_new failed");
    }
    if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
        return fail_out(out_plaintext, err, "AES-GCM decrypt init failed: " + last_openssl_error());
    }

    if (!aad.empty() && !feed_aad(ctx.get(), aad, false)) {
        return fail_out(out_plaintext, err, "AES-GCM AAD failed: " + last_openssl_error());
    }

    std::string pt(ciphertext.size(), '\0');
    std::size_t total = 0;
    std::size_t off = 0;
    while (off < ciphertext.size()) {
        int n = static_cast<int>((ciphertext.size() - off) < kEvpChunkSize
                                     ? (ciphertext.size() - off) : kEvpChunkSize);
        int outl = 0;
        if (EVP_DecryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(&pt[total]), &outl,
                              reinterpret_cast<const unsigned char*>(ciphertext.data() + off),
                              n) != 1) {
            secure_zero(pt);
            return fail_out(out_plaintext, err, "AES-GCM decrypt update failed: " + last_openssl_error());
        }
        total += static_cast<std::size_t>(outl);
        off += static_cast<std::size_t>(n);
    }

    std::string tag_copy = tag;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(kAesGcmTagSize), &tag_copy[0]) != 1) {
        secure_zero(pt);
        return fail_out(out_plaintext, err, "AES-GCM set tag failed: " + last_openssl_error());
    }

    unsigned char final_buf[kAesBlockSize];
    int finl = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), final_buf, &finl) != 1) {
        secure_zero(pt);
        secure_zero(final_buf, sizeof(final_buf));
        return fail_out(out_plaintext, err, "AES-GCM authentication failed");
    }
    if (finl > 0) {
        pt.resize(total);
        pt.append(reinterpret_cast<const char*>(final_buf), static_cast<std::size_t>(finl));
    } else {
        pt.resize(total);
    }
    secure_zero(final_buf, sizeof(final_buf));
    out_plaintext.swap(pt);
    return true;
}

bool aes_gcm_seal(const std::string& key,
                  const std::string& plaintext,
                  std::string& out_sealed,
                  const std::string& aad,
                  std::string* err) {
    std::string nonce;
    if (!random_bytes(kAesGcmNonceSize, nonce, err)) {
        out_sealed.clear();
        return false;
    }

    std::string ct, tag;
    if (!aes_gcm_encrypt(key, nonce, plaintext, ct, tag, aad, err)) {
        out_sealed.clear();
        return false;
    }

    std::string sealed;
    sealed.reserve(nonce.size() + tag.size() + ct.size());
    sealed.append(nonce);
    sealed.append(tag);
    sealed.append(ct);
    out_sealed.swap(sealed);
    return true;
}

bool aes_gcm_open(const std::string& key,
                  const std::string& sealed,
                  std::string& out_plaintext,
                  const std::string& aad,
                  std::string* err) {
    const std::size_t overhead = kAesGcmNonceSize + kAesGcmTagSize;
    if (sealed.size() < overhead) {
        return fail_out(out_plaintext, err, "sealed AES-GCM payload is too short");
    }
    std::string nonce = sealed.substr(0, kAesGcmNonceSize);
    std::string tag   = sealed.substr(kAesGcmNonceSize, kAesGcmTagSize);
    std::string ct    = sealed.substr(overhead);
    return aes_gcm_decrypt(key, nonce, ct, tag, out_plaintext, aad, err);
}

bool aes_gcm_seal_base64(const std::string& key,
                         const std::string& plaintext,
                         std::string& out_b64,
                         const std::string& aad,
                         std::string* err) {
    std::string sealed;
    if (!aes_gcm_seal(key, plaintext, sealed, aad, err)) {
        out_b64.clear();
        return false;
    }
    out_b64 = base64_encode(sealed);
    return true;
}

bool aes_gcm_open_base64(const std::string& key,
                         const std::string& b64_sealed,
                         std::string& out_plaintext,
                         const std::string& aad,
                         std::string* err) {
    std::string sealed;
    if (!base64_decode(b64_sealed, sealed, err)) {
        out_plaintext.clear();
        return false;
    }
    return aes_gcm_open(key, sealed, out_plaintext, aad, err);
}

bool aes_cbc_encrypt(const std::string& key,
                     const std::string& iv16,
                     const std::string& plaintext,
                     std::string& out_ciphertext,
                     std::string* err) {
    const EVP_CIPHER* cipher = aes_cbc_cipher(key.size());
    if (!cipher) {
        return fail_out(out_ciphertext, err, "AES key length must be 16, 24, or 32 bytes");
    }
    if (iv16.size() != kAesBlockSize) {
        return fail_out(out_ciphertext, err, "AES-CBC IV must be 16 bytes");
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return fail_out(out_ciphertext, err, "EVP_CIPHER_CTX_new failed");
    }
    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv16.data())) != 1) {
        return fail_out(out_ciphertext, err, "AES-CBC encrypt init failed: " + last_openssl_error());
    }

    std::string ct(plaintext.size() + kAesBlockSize, '\0');
    std::size_t total = 0;
    std::size_t off = 0;
    while (off < plaintext.size()) {
        int n = static_cast<int>((plaintext.size() - off) < kEvpChunkSize
                                     ? (plaintext.size() - off) : kEvpChunkSize);
        int outl = 0;
        if (EVP_EncryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(&ct[total]), &outl,
                              reinterpret_cast<const unsigned char*>(plaintext.data() + off),
                              n) != 1) {
            return fail_out(out_ciphertext, err, "AES-CBC encrypt update failed: " + last_openssl_error());
        }
        total += static_cast<std::size_t>(outl);
        off += static_cast<std::size_t>(n);
    }

    int finl = 0;
    if (EVP_EncryptFinal_ex(ctx.get(),
                            reinterpret_cast<unsigned char*>(&ct[total]), &finl) != 1) {
        return fail_out(out_ciphertext, err, "AES-CBC encrypt final failed: " + last_openssl_error());
    }
    ct.resize(total + static_cast<std::size_t>(finl));
    out_ciphertext.swap(ct);
    return true;
}

bool aes_cbc_decrypt(const std::string& key,
                     const std::string& iv16,
                     const std::string& ciphertext,
                     std::string& out_plaintext,
                     std::string* err) {
    const EVP_CIPHER* cipher = aes_cbc_cipher(key.size());
    if (!cipher) {
        return fail_out(out_plaintext, err, "AES key length must be 16, 24, or 32 bytes");
    }
    if (iv16.size() != kAesBlockSize) {
        return fail_out(out_plaintext, err, "AES-CBC IV must be 16 bytes");
    }
    if (ciphertext.empty() || (ciphertext.size() % kAesBlockSize) != 0u) {
        return fail_out(out_plaintext, err, "AES-CBC ciphertext length must be a non-zero multiple of 16");
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return fail_out(out_plaintext, err, "EVP_CIPHER_CTX_new failed");
    }
    if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv16.data())) != 1) {
        return fail_out(out_plaintext, err, "AES-CBC decrypt init failed: " + last_openssl_error());
    }

    std::string pt(ciphertext.size() + kAesBlockSize, '\0');
    std::size_t total = 0;
    std::size_t off = 0;
    while (off < ciphertext.size()) {
        int n = static_cast<int>((ciphertext.size() - off) < kEvpChunkSize
                                     ? (ciphertext.size() - off) : kEvpChunkSize);
        int outl = 0;
        if (EVP_DecryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(&pt[total]), &outl,
                              reinterpret_cast<const unsigned char*>(ciphertext.data() + off),
                              n) != 1) {
            secure_zero(pt);
            return fail_out(out_plaintext, err, "AES-CBC decrypt update failed: " + last_openssl_error());
        }
        total += static_cast<std::size_t>(outl);
        off += static_cast<std::size_t>(n);
    }

    int finl = 0;
    if (EVP_DecryptFinal_ex(ctx.get(),
                            reinterpret_cast<unsigned char*>(&pt[total]), &finl) != 1) {
        secure_zero(pt);
        return fail_out(out_plaintext, err, "AES-CBC decrypt final failed (bad padding or key)");
    }
    pt.resize(total + static_cast<std::size_t>(finl));
    out_plaintext.swap(pt);
    return true;
}

bool aes_ctr_crypt(const std::string& key,
                   const std::string& iv16,
                   const std::string& input,
                   std::string& out,
                   std::string* err) {
    const EVP_CIPHER* cipher = aes_ctr_cipher(key.size());
    if (!cipher) {
        return fail_out(out, err, "AES key length must be 16, 24, or 32 bytes");
    }
    if (iv16.size() != kAesBlockSize) {
        return fail_out(out, err, "AES-CTR IV must be 16 bytes");
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return fail_out(out, err, "EVP_CIPHER_CTX_new failed");
    }
    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv16.data())) != 1) {
        return fail_out(out, err, "AES-CTR init failed: " + last_openssl_error());
    }

    std::string tmp(input.size(), '\0');
    std::size_t total = 0;
    std::size_t off = 0;
    while (off < input.size()) {
        int n = static_cast<int>((input.size() - off) < kEvpChunkSize
                                     ? (input.size() - off) : kEvpChunkSize);
        int outl = 0;
        if (EVP_EncryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(&tmp[total]), &outl,
                              reinterpret_cast<const unsigned char*>(input.data() + off),
                              n) != 1) {
            secure_zero(tmp);
            return fail_out(out, err, "AES-CTR update failed: " + last_openssl_error());
        }
        total += static_cast<std::size_t>(outl);
        off += static_cast<std::size_t>(n);
    }

    unsigned char final_buf[kAesBlockSize];
    int finl = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), final_buf, &finl) != 1) {
        secure_zero(tmp);
        return fail_out(out, err, "AES-CTR final failed: " + last_openssl_error());
    }
    if (finl > 0) {
        tmp.resize(total);
        tmp.append(reinterpret_cast<const char*>(final_buf), static_cast<std::size_t>(finl));
    } else {
        tmp.resize(total);
    }
    secure_zero(final_buf, sizeof(final_buf));
    out.swap(tmp);
    return true;
}

} // namespace crypto
} // namespace myframe
