#pragma once

#include <string>

namespace myframe {
namespace crypto {

// PEM keypair generation (PKCS#8 Private Key + X.509 SubjectPublicKeyInfo Public Key).
// RSA requires bits >= 2048.
// On any error, output strings are cleared and false is returned.
// Keypair generators require distinct output references (&out_private_pem != &out_public_pem).
// Single-output encrypt/decrypt/sign APIs support input-output aliasing.
bool generate_rsa_keypair(int bits,
                          std::string& out_private_pem,
                          std::string& out_public_pem,
                          std::string* err = nullptr);

bool generate_ec_p256_keypair(std::string& out_private_pem,
                              std::string& out_public_pem,
                              std::string* err = nullptr);

bool generate_ed25519_keypair(std::string& out_private_pem,
                              std::string& out_public_pem,
                              std::string* err = nullptr);

// RSA-OAEP with SHA-256 (MGF1-SHA256).
bool rsa_oaep_encrypt(const std::string& public_pem,
                      const std::string& plaintext,
                      std::string& out_ciphertext,
                      std::string* err = nullptr);

bool rsa_oaep_decrypt(const std::string& private_pem,
                      const std::string& ciphertext,
                      std::string& out_plaintext,
                      std::string* err = nullptr);

// RSA-PSS with SHA-256 (salt length = digest length).
bool rsa_pss_sign(const std::string& private_pem,
                  const std::string& message,
                  std::string& out_sig,
                  std::string* err = nullptr);

bool rsa_pss_verify(const std::string& public_pem,
                    const std::string& message,
                    const std::string& sig,
                    std::string* err = nullptr);

// ECDSA over P-256 (or compatible EC key) with SHA-256 (ASN.1 DER signature).
bool ecdsa_sha256_sign(const std::string& private_pem,
                       const std::string& message,
                       std::string& out_sig,
                       std::string* err = nullptr);

bool ecdsa_sha256_verify(const std::string& public_pem,
                         const std::string& message,
                         const std::string& sig,
                         std::string* err = nullptr);

// Ed25519 pure signing / verification (64-byte raw signature).
bool ed25519_sign(const std::string& private_pem,
                  const std::string& message,
                  std::string& out_sig,
                  std::string* err = nullptr);

bool ed25519_verify(const std::string& public_pem,
                    const std::string& message,
                    const std::string& sig,
                    std::string* err = nullptr);

} // namespace crypto
} // namespace myframe
