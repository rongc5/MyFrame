#pragma once

#include <cstddef>
#include <string>

namespace myframe {
namespace crypto {

static const std::size_t kAesBlockSize    = 16;
static const std::size_t kAesGcmNonceSize = 12;
static const std::size_t kAesGcmTagSize   = 16;

// All AES helpers automatically select AES-128 / AES-192 / AES-256 based on key.size()
// (16, 24, or 32 bytes). On any error, the output string is cleared and false is returned.

// ---------------------------------------------------------------------------
// Recommended default API: self-contained AES-GCM AEAD box
// Wire format: [ 12-byte random nonce | 16-byte GCM auth tag | ciphertext ]
// ---------------------------------------------------------------------------
bool aes_gcm_seal(const std::string& key,
                  const std::string& plaintext,
                  std::string& out_sealed,
                  const std::string& aad = std::string(),
                  std::string* err = nullptr);

bool aes_gcm_open(const std::string& key,
                  const std::string& sealed,
                  std::string& out_plaintext,
                  const std::string& aad = std::string(),
                  std::string* err = nullptr);

// Base64-encoded variants of aes_gcm_seal / aes_gcm_open (handy for JSON/DB/config fields).
bool aes_gcm_seal_base64(const std::string& key,
                         const std::string& plaintext,
                         std::string& out_b64,
                         const std::string& aad = std::string(),
                         std::string* err = nullptr);

bool aes_gcm_open_base64(const std::string& key,
                         const std::string& b64_sealed,
                         std::string& out_plaintext,
                         const std::string& aad = std::string(),
                         std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Explicit-parameter AES-GCM (for interop with external protocols & test vectors)
// ---------------------------------------------------------------------------
bool aes_gcm_encrypt(const std::string& key,
                     const std::string& nonce,
                     const std::string& plaintext,
                     std::string& out_ciphertext,
                     std::string& out_tag,
                     const std::string& aad = std::string(),
                     std::string* err = nullptr);

bool aes_gcm_decrypt(const std::string& key,
                     const std::string& nonce,
                     const std::string& ciphertext,
                     const std::string& tag,
                     std::string& out_plaintext,
                     const std::string& aad = std::string(),
                     std::string* err = nullptr);

// ---------------------------------------------------------------------------
// AES-CBC (PKCS#7 padding, 16-byte IV) & AES-CTR (16-byte IV/counter)
// Use only when interoperating with legacy or external protocols; prefer AES-GCM otherwise.
// ---------------------------------------------------------------------------
bool aes_cbc_encrypt(const std::string& key,
                     const std::string& iv16,
                     const std::string& plaintext,
                     std::string& out_ciphertext,
                     std::string* err = nullptr);

bool aes_cbc_decrypt(const std::string& key,
                     const std::string& iv16,
                     const std::string& ciphertext,
                     std::string& out_plaintext,
                     std::string* err = nullptr);

bool aes_ctr_crypt(const std::string& key,
                   const std::string& iv16,
                   const std::string& input,
                   std::string& out,
                   std::string* err = nullptr);

} // namespace crypto
} // namespace myframe
