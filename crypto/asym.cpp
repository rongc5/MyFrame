#include "asym.h"
#include "random.h"

#include <climits>
#include <memory>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace myframe {
namespace crypto {

namespace {

struct BioDeleter     { void operator()(BIO* p) const { if (p) BIO_free_all(p); } };
struct PkeyDeleter    { void operator()(EVP_PKEY* p) const { if (p) EVP_PKEY_free(p); } };
struct PkeyCtxDeleter { void operator()(EVP_PKEY_CTX* p) const { if (p) EVP_PKEY_CTX_free(p); } };
struct MdCtxDeleter   { void operator()(EVP_MD_CTX* p) const { if (p) EVP_MD_CTX_free(p); } };

typedef std::unique_ptr<BIO, BioDeleter>             BioPtr;
typedef std::unique_ptr<EVP_PKEY, PkeyDeleter>       PkeyPtr;
typedef std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter> PkeyCtxPtr;
typedef std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>    MdCtxPtr;

static void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

static bool fail_out(std::string& out, std::string* err, const std::string& msg) {
    out.clear();
    set_err(err, msg);
    return false;
}

static bool fail_out2(std::string& out1,
                      std::string& out2,
                      std::string* err,
                      const std::string& msg) {
    out1.clear();
    out2.clear();
    set_err(err, msg);
    return false;
}

static std::string last_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "openssl asymmetric error";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

// Never prompt stdin if someone passes a passphrase-protected PEM.
static int no_passphrase_cb(char*, int, int, void*) {
    return 0;
}

static PkeyPtr load_private_pem(const std::string& pem, std::string* err) {
    if (pem.empty() || pem.size() > static_cast<std::size_t>(INT_MAX)) {
        set_err(err, "invalid private key PEM");
        return PkeyPtr();
    }
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        set_err(err, "BIO_new_mem_buf failed");
        return PkeyPtr();
    }
    PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, no_passphrase_cb, nullptr));
    if (!key) {
        set_err(err, "failed to parse private key PEM: " + last_openssl_error());
    }
    return key;
}

static PkeyPtr load_public_pem(const std::string& pem, std::string* err) {
    if (pem.empty() || pem.size() > static_cast<std::size_t>(INT_MAX)) {
        set_err(err, "invalid public key PEM");
        return PkeyPtr();
    }
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) {
        set_err(err, "BIO_new_mem_buf failed");
        return PkeyPtr();
    }
    PkeyPtr key(PEM_read_bio_PUBKEY(bio.get(), nullptr, no_passphrase_cb, nullptr));
    if (!key) {
        set_err(err, "failed to parse public key PEM: " + last_openssl_error());
    }
    return key;
}

static bool export_keypair_pem(EVP_PKEY* pkey,
                               std::string& out_private_pem,
                               std::string& out_public_pem,
                               std::string* err) {
    if (&out_private_pem == &out_public_pem) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "output private and public PEM references must be distinct");
    }

    BioPtr priv_bio(BIO_new(BIO_s_mem()));
    BioPtr pub_bio(BIO_new(BIO_s_mem()));
    if (!priv_bio || !pub_bio) {
        return fail_out2(out_private_pem, out_public_pem, err, "BIO_new failed");
    }
    if (PEM_write_bio_PKCS8PrivateKey(priv_bio.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "PEM_write_bio_PKCS8PrivateKey failed: " + last_openssl_error());
    }
    if (PEM_write_bio_PUBKEY(pub_bio.get(), pkey) != 1) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "PEM_write_bio_PUBKEY failed: " + last_openssl_error());
    }

    char* priv_data = nullptr;
    long priv_len = BIO_get_mem_data(priv_bio.get(), &priv_data);
    char* pub_data = nullptr;
    long pub_len = BIO_get_mem_data(pub_bio.get(), &pub_data);
    if (priv_len <= 0 || !priv_data || pub_len <= 0 || !pub_data) {
        return fail_out2(out_private_pem, out_public_pem, err, "failed to read exported PEM data");
    }

    std::string priv_pem(priv_data, static_cast<std::size_t>(priv_len));
    std::string pub_pem(pub_data, static_cast<std::size_t>(pub_len));
    out_private_pem.swap(priv_pem);
    out_public_pem.swap(pub_pem);
    return true;
}

} // namespace

bool generate_rsa_keypair(int bits,
                          std::string& out_private_pem,
                          std::string& out_public_pem,
                          std::string* err) {
    if (&out_private_pem == &out_public_pem) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "output private and public PEM references must be distinct");
    }
    if (bits < 2048 || bits > 8192) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "RSA key size must be in [2048, 8192] bits");
    }
    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!kctx ||
        EVP_PKEY_keygen_init(kctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx.get(), bits) <= 0) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "RSA keygen init failed: " + last_openssl_error());
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_key) <= 0 || !raw_key) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "RSA keygen failed: " + last_openssl_error());
    }
    PkeyPtr key(raw_key);
    return export_keypair_pem(key.get(), out_private_pem, out_public_pem, err);
}

bool generate_ec_p256_keypair(std::string& out_private_pem,
                              std::string& out_public_pem,
                              std::string* err) {
    if (&out_private_pem == &out_public_pem) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "output private and public PEM references must be distinct");
    }
    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!kctx ||
        EVP_PKEY_keygen_init(kctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx.get(), NID_X9_62_prime256v1) <= 0) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "EC P-256 keygen init failed: " + last_openssl_error());
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_key) <= 0 || !raw_key) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "EC P-256 keygen failed: " + last_openssl_error());
    }
    PkeyPtr key(raw_key);
    return export_keypair_pem(key.get(), out_private_pem, out_public_pem, err);
}

bool generate_ed25519_keypair(std::string& out_private_pem,
                              std::string& out_public_pem,
                              std::string* err) {
    if (&out_private_pem == &out_public_pem) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "output private and public PEM references must be distinct");
    }
#ifdef EVP_PKEY_ED25519
    PkeyCtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr));
    if (!kctx || EVP_PKEY_keygen_init(kctx.get()) <= 0) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "Ed25519 keygen init failed: " + last_openssl_error());
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_key) <= 0 || !raw_key) {
        return fail_out2(out_private_pem, out_public_pem, err,
                         "Ed25519 keygen failed: " + last_openssl_error());
    }
    PkeyPtr key(raw_key);
    return export_keypair_pem(key.get(), out_private_pem, out_public_pem, err);
#else
    return fail_out2(out_private_pem, out_public_pem, err,
                     "Ed25519 is not supported by this OpenSSL version");
#endif
}

bool rsa_oaep_encrypt(const std::string& public_pem,
                      const std::string& plaintext,
                      std::string& out_ciphertext,
                      std::string* err) {
    PkeyPtr key = load_public_pem(public_pem, err);
    if (!key) { out_ciphertext.clear(); return false; }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
        out_ciphertext.clear();
        set_err(err, "expected an RSA public key");
        return false;
    }
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
    if (!ctx ||
        EVP_PKEY_encrypt_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) <= 0) {
        out_ciphertext.clear();
        set_err(err, "RSA-OAEP encrypt init failed: " + last_openssl_error());
        return false;
    }
    static const unsigned char kEmpty = 0;
    const unsigned char* in = plaintext.empty()
        ? &kEmpty : reinterpret_cast<const unsigned char*>(plaintext.data());
    std::size_t out_len = 0;
    if (EVP_PKEY_encrypt(ctx.get(), nullptr, &out_len, in, plaintext.size()) <= 0) {
        out_ciphertext.clear();
        set_err(err, "RSA-OAEP encrypt size query failed: " + last_openssl_error());
        return false;
    }
    std::string ct(out_len, '\0');
    if (EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<unsigned char*>(&ct[0]),
                         &out_len, in, plaintext.size()) <= 0) {
        out_ciphertext.clear();
        set_err(err, "RSA-OAEP encrypt failed: " + last_openssl_error());
        return false;
    }
    ct.resize(out_len);
    out_ciphertext.swap(ct);
    return true;
}

bool rsa_oaep_decrypt(const std::string& private_pem,
                      const std::string& ciphertext,
                      std::string& out_plaintext,
                      std::string* err) {
    PkeyPtr key = load_private_pem(private_pem, err);
    if (!key) { out_plaintext.clear(); return false; }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
        out_plaintext.clear();
        set_err(err, "expected an RSA private key");
        return false;
    }
    if (ciphertext.empty()) {
        out_plaintext.clear();
        set_err(err, "RSA-OAEP ciphertext is empty");
        return false;
    }
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
    if (!ctx ||
        EVP_PKEY_decrypt_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) <= 0) {
        out_plaintext.clear();
        set_err(err, "RSA-OAEP decrypt init failed: " + last_openssl_error());
        return false;
    }
    const unsigned char* in = reinterpret_cast<const unsigned char*>(ciphertext.data());
    std::size_t out_len = 0;
    if (EVP_PKEY_decrypt(ctx.get(), nullptr, &out_len, in, ciphertext.size()) <= 0) {
        out_plaintext.clear();
        set_err(err, "RSA-OAEP decrypt size query failed: " + last_openssl_error());
        return false;
    }
    std::string pt(out_len, '\0');
    if (EVP_PKEY_decrypt(ctx.get(), reinterpret_cast<unsigned char*>(&pt[0]),
                         &out_len, in, ciphertext.size()) <= 0) {
        secure_zero(pt);
        out_plaintext.clear();
        set_err(err, "RSA-OAEP decrypt failed: " + last_openssl_error());
        return false;
    }
    pt.resize(out_len);
    out_plaintext.swap(pt);
    return true;
}

bool rsa_pss_sign(const std::string& private_pem,
                  const std::string& message,
                  std::string& out_sig,
                  std::string* err) {
    PkeyPtr key = load_private_pem(private_pem, err);
    if (!key) {
        out_sig.clear();
        return false;
    }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
        return fail_out(out_sig, err, "expected an RSA private key");
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    EVP_PKEY_CTX* pctx = nullptr;
    if (!mctx ||
        EVP_DigestSignInit(mctx.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) <= 0) {
        return fail_out(out_sig, err, "RSA-PSS sign init failed: " + last_openssl_error());
    }
    if (!message.empty() && EVP_DigestSignUpdate(mctx.get(), message.data(), message.size()) != 1) {
        return fail_out(out_sig, err, "RSA-PSS sign update failed: " + last_openssl_error());
    }
    std::size_t sig_len = 0;
    if (EVP_DigestSignFinal(mctx.get(), nullptr, &sig_len) != 1 || sig_len == 0) {
        return fail_out(out_sig, err, "RSA-PSS sign size query failed: " + last_openssl_error());
    }
    std::string sig(sig_len, '\0');
    if (EVP_DigestSignFinal(mctx.get(), reinterpret_cast<unsigned char*>(&sig[0]), &sig_len) != 1) {
        return fail_out(out_sig, err, "RSA-PSS sign final failed: " + last_openssl_error());
    }
    sig.resize(sig_len);
    out_sig.swap(sig);
    return true;
}

bool rsa_pss_verify(const std::string& public_pem,
                    const std::string& message,
                    const std::string& sig,
                    std::string* err) {
    PkeyPtr key = load_public_pem(public_pem, err);
    if (!key) return false;
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
        set_err(err, "expected an RSA public key");
        return false;
    }
    if (sig.empty()) {
        set_err(err, "signature is empty");
        return false;
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    EVP_PKEY_CTX* pctx = nullptr;
    if (!mctx ||
        EVP_DigestVerifyInit(mctx.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) <= 0) {
        set_err(err, "RSA-PSS verify init failed: " + last_openssl_error());
        return false;
    }
    if (!message.empty() && EVP_DigestVerifyUpdate(mctx.get(), message.data(), message.size()) != 1) {
        set_err(err, "RSA-PSS verify update failed: " + last_openssl_error());
        return false;
    }
    int rc = EVP_DigestVerifyFinal(mctx.get(),
                                   reinterpret_cast<const unsigned char*>(sig.data()),
                                   sig.size());
    if (rc != 1) {
        set_err(err, "RSA-PSS signature verification failed");
        return false;
    }
    return true;
}

bool ecdsa_sha256_sign(const std::string& private_pem,
                       const std::string& message,
                       std::string& out_sig,
                       std::string* err) {
    PkeyPtr key = load_private_pem(private_pem, err);
    if (!key) {
        out_sig.clear();
        return false;
    }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC) {
        return fail_out(out_sig, err, "expected an EC private key");
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    if (!mctx || EVP_DigestSignInit(mctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        return fail_out(out_sig, err, "ECDSA sign init failed: " + last_openssl_error());
    }
    if (!message.empty() && EVP_DigestSignUpdate(mctx.get(), message.data(), message.size()) != 1) {
        return fail_out(out_sig, err, "ECDSA sign update failed: " + last_openssl_error());
    }
    std::size_t sig_len = 0;
    if (EVP_DigestSignFinal(mctx.get(), nullptr, &sig_len) != 1 || sig_len == 0) {
        return fail_out(out_sig, err, "ECDSA sign size query failed: " + last_openssl_error());
    }
    std::string sig(sig_len, '\0');
    if (EVP_DigestSignFinal(mctx.get(), reinterpret_cast<unsigned char*>(&sig[0]), &sig_len) != 1) {
        return fail_out(out_sig, err, "ECDSA sign final failed: " + last_openssl_error());
    }
    sig.resize(sig_len);
    out_sig.swap(sig);
    return true;
}

bool ecdsa_sha256_verify(const std::string& public_pem,
                         const std::string& message,
                         const std::string& sig,
                         std::string* err) {
    PkeyPtr key = load_public_pem(public_pem, err);
    if (!key) return false;
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC) {
        set_err(err, "expected an EC public key");
        return false;
    }
    if (sig.empty()) {
        set_err(err, "signature is empty");
        return false;
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    if (!mctx || EVP_DigestVerifyInit(mctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        set_err(err, "ECDSA verify init failed: " + last_openssl_error());
        return false;
    }
    if (!message.empty() && EVP_DigestVerifyUpdate(mctx.get(), message.data(), message.size()) != 1) {
        set_err(err, "ECDSA verify update failed: " + last_openssl_error());
        return false;
    }
    int rc = EVP_DigestVerifyFinal(mctx.get(),
                                   reinterpret_cast<const unsigned char*>(sig.data()),
                                   sig.size());
    if (rc != 1) {
        set_err(err, "ECDSA signature verification failed");
        return false;
    }
    return true;
}

bool ed25519_sign(const std::string& private_pem,
                  const std::string& message,
                  std::string& out_sig,
                  std::string* err) {
#ifdef EVP_PKEY_ED25519
    PkeyPtr key = load_private_pem(private_pem, err);
    if (!key) {
        out_sig.clear();
        return false;
    }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        return fail_out(out_sig, err, "expected an Ed25519 private key");
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    if (!mctx || EVP_DigestSignInit(mctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return fail_out(out_sig, err, "Ed25519 sign init failed: " + last_openssl_error());
    }
    static const unsigned char kEmpty = 0;
    const unsigned char* msg = message.empty()
        ? &kEmpty : reinterpret_cast<const unsigned char*>(message.data());
    std::size_t sig_len = 0;
    if (EVP_DigestSign(mctx.get(), nullptr, &sig_len, msg, message.size()) != 1 || sig_len == 0) {
        return fail_out(out_sig, err, "Ed25519 sign size query failed: " + last_openssl_error());
    }
    std::string sig(sig_len, '\0');
    if (EVP_DigestSign(mctx.get(), reinterpret_cast<unsigned char*>(&sig[0]),
                       &sig_len, msg, message.size()) != 1) {
        return fail_out(out_sig, err, "Ed25519 sign failed: " + last_openssl_error());
    }
    sig.resize(sig_len);
    out_sig.swap(sig);
    return true;
#else
    (void)private_pem; (void)message;
    return fail_out(out_sig, err, "Ed25519 is not supported by this OpenSSL version");
#endif
}

bool ed25519_verify(const std::string& public_pem,
                    const std::string& message,
                    const std::string& sig,
                    std::string* err) {
#ifdef EVP_PKEY_ED25519
    PkeyPtr key = load_public_pem(public_pem, err);
    if (!key) return false;
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        set_err(err, "expected an Ed25519 public key");
        return false;
    }
    if (sig.size() != 64u) {
        set_err(err, "Ed25519 signature must be 64 bytes");
        return false;
    }
    MdCtxPtr mctx(EVP_MD_CTX_new());
    if (!mctx || EVP_DigestVerifyInit(mctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        set_err(err, "Ed25519 verify init failed: " + last_openssl_error());
        return false;
    }
    static const unsigned char kEmpty = 0;
    const unsigned char* msg = message.empty()
        ? &kEmpty : reinterpret_cast<const unsigned char*>(message.data());
    int rc = EVP_DigestVerify(mctx.get(),
                              reinterpret_cast<const unsigned char*>(sig.data()),
                              sig.size(), msg, message.size());
    if (rc != 1) {
        set_err(err, "Ed25519 signature verification failed");
        return false;
    }
    return true;
#else
    (void)public_pem; (void)message; (void)sig;
    set_err(err, "Ed25519 is not supported by this OpenSSL version");
    return false;
#endif
}

} // namespace crypto
} // namespace myframe
