// Unit, NIST/RFC vector, tamper-resistance, and concurrency test suite for myframe::crypto (C++11).
#include "crypto/crypto.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_checks(0);
std::atomic<int> g_fails(0);

#define CHECK(expr)                                                            \
    do {                                                                       \
        g_checks.fetch_add(1);                                                 \
        if (!(expr)) {                                                         \
            g_fails.fetch_add(1);                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                      \
    } while (0)

std::string from_hex(const std::string& h) {
    std::string out;
    bool ok = myframe::crypto::hex_decode(h, out);
    CHECK(ok);
    return out;
}

void test_random_and_codec() {
    using namespace myframe::crypto;

    // CSPRNG uniqueness and length
    std::string r1, r2;
    CHECK(random_bytes(32, r1));
    CHECK(random_bytes(32, r2));
    CHECK(r1.size() == 32 && r2.size() == 32 && r1 != r2);
    CHECK(random_bytes(0, r1) && r1.empty());
    CHECK(random_hex(16).size() == 32);
    CHECK(!random_base64url(24).empty());
    CHECK(random_base64url(24).find('=') == std::string::npos);

    // Constant-time compare & secure_zero
    CHECK(secure_equal("abc", "abc"));
    CHECK(!secure_equal("abc", "abd"));
    CHECK(!secure_equal("abc", "abcd"));
    CHECK(secure_equal("", ""));
    std::string secret = "super-secret-key-material";
    secure_zero(secret);
    CHECK(secret.empty());

    // Hex codec
    std::string all_bytes;
    all_bytes.resize(256);
    for (int i = 0; i < 256; ++i) all_bytes[i] = static_cast<char>(i);
    std::string hx = hex_encode(all_bytes);
    CHECK(hx.size() == 512);
    std::string dec;
    CHECK(hex_decode(hx, dec) && dec == all_bytes);
    CHECK(hex_decode(hex_encode(all_bytes, true), dec) && dec == all_bytes);
    std::string err;
    CHECK(!hex_decode("abc", dec, &err) && dec.empty() && !err.empty());
    CHECK(!hex_decode("zz", dec, &err) && dec.empty());

    // RFC 4648 Section 10 Base64 vectors
    const char* raw_v[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* b64_v[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (std::size_t i = 0; i < 7; ++i) {
        std::string enc = base64_encode(std::string(raw_v[i]));
        CHECK(enc == b64_v[i]);
        CHECK(base64_decode(enc, dec) && dec == raw_v[i]);
    }
    // Full 256-byte round-trip (verifies unsigned char handling for 0x80..0xFF)
    CHECK(base64_decode(base64_encode(all_bytes), dec) && dec == all_bytes);
    CHECK(base64url_decode(base64url_encode(all_bytes, false), dec) && dec == all_bytes);
    CHECK(base64url_decode(base64url_encode(all_bytes, true), dec) && dec == all_bytes);
    // Strict Base64 rejections
    CHECK(!base64_decode("abc", dec));         // bad length
    CHECK(!base64_decode("Zm9*", dec));        // bad char
    CHECK(!base64_decode("Zh==", dec));        // non-zero trailing bits
    CHECK(!base64url_decode("abcde", dec));    // len % 4 == 1
    CHECK(!base64url_decode("ab+c", dec));     // '+' forbidden in base64url
}

void test_hash_and_hmac() {
    using namespace myframe::crypto;

    // FIPS 180-4 / RFC 1321 vectors
    CHECK(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha512_hex("abc") ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");

    // Streaming Hasher + reset
    Hasher h(HashAlg::SHA256);
    CHECK(h.ok());
    CHECK(h.update("a") && h.update("bc"));
    std::string hex;
    CHECK(h.final_hex(hex) && hex == sha256_hex("abc"));
    CHECK(!h.update("more")); // finalized until reset()
    CHECK(h.reset() && h.update("hello") && h.final_hex(hex) && hex == sha256_hex("hello"));

    // File hashing (including in-place path/output aliasing)
    const std::string tmp_path = "/tmp/myframe_crypto_hash_test.bin";
    {
        std::ofstream ofs(tmp_path.c_str(), std::ios::binary);
        ofs << "abc";
    }
    CHECK(hash_file_hex(HashAlg::SHA256, tmp_path, hex) && hex == sha256_hex("abc"));
    {
        std::string alias_path = tmp_path;
        CHECK(hash_file_hex(HashAlg::SHA256, alias_path, alias_path) &&
              alias_path == sha256_hex("abc"));
    }
    std::remove(tmp_path.c_str());
    CHECK(!hash_file_hex(HashAlg::SHA256, "/tmp/nonexistent_myframe_crypto_xyz", hex) && hex.empty());

    // RFC 4231 HMAC-SHA256 & HMAC-SHA512 Test Case 2
    const std::string key = "Jefe";
    const std::string msg = "what do ya want for nothing?";
    const std::string exp256 = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    const std::string exp512 =
        "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
        "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737";
    CHECK(hmac_sha256_hex(key, msg) == exp256);
    CHECK(hmac_sha512_hex(key, msg) == exp512);
    // In-place aliasing for hmac_raw (&data == &out and &key == &out)
    // and rejection of null pointers paired with positive lengths.
    {
        std::string alias_msg = msg;
        CHECK(hmac_raw(HashAlg::SHA256, key, alias_msg, alias_msg) &&
              hex_encode(alias_msg) == exp256);
        std::string alias_key = key;
        CHECK(hmac_raw(HashAlg::SHA256, alias_key, msg, alias_key) &&
              hex_encode(alias_key) == exp256);

        std::string stale = "stale-mac", hmac_err;
        CHECK(!hmac_raw(HashAlg::SHA256, nullptr, 4, msg.data(), msg.size(), stale, &hmac_err));
        CHECK(stale.empty() && !hmac_err.empty());
        stale = "stale-mac";
        hmac_err.clear();
        CHECK(!hmac_raw(HashAlg::SHA256, key.data(), key.size(), nullptr, 4, stale, &hmac_err));
        CHECK(stale.empty() && !hmac_err.empty());
        // Zero-length null key/data remains allowed (matches empty key/data).
        std::string empty_mac;
        CHECK(hmac_raw(HashAlg::SHA256, nullptr, 0, nullptr, 0, empty_mac, &hmac_err) &&
              hex_encode(empty_mac) == hmac_sha256_hex("", ""));
    }
    CHECK(hmac_sha256_verify_hex(key, msg, exp256));
    CHECK(hmac_sha256_verify_hex(key, msg, hex_encode(from_hex(exp256), true))); // uppercase hex
    CHECK(!hmac_sha256_verify_hex(key, msg + "!", exp256));
    CHECK(!hmac_sha256_verify_hex("wrong", msg, exp256));
    CHECK(hmac_sha512_verify_hex(key, msg, exp512));
    CHECK(!hmac_sha512_verify_hex(key, "tampered", exp512));
}

void test_kdf() {
    using namespace myframe::crypto;

    // RFC 5869 HKDF-SHA256 Test Case 1
    std::string okm;
    CHECK(hkdf_sha256(
        from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
        from_hex("000102030405060708090a0b0c"),
        from_hex("f0f1f2f3f4f5f6f7f8f9"),
        42, okm));
    CHECK(hex_encode(okm) ==
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    // RFC 5869 Test Case 3 (empty salt & info)
    CHECK(hkdf_sha256(
        from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
        "", "", 42, okm));
    CHECK(hex_encode(okm) ==
          "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");

    // Bounds
    CHECK(!hkdf_sha256("ikm", "s", "i", 0, okm) && okm.empty());
    CHECK(!hkdf_sha256("ikm", "s", "i", 255 * 32 + 1, okm) && okm.empty());

    // PBKDF2-HMAC-SHA256 standard vectors
    std::string dk;
    CHECK(pbkdf2_hmac_sha256("password", "salt", 1, 32, dk));
    CHECK(hex_encode(dk) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(pbkdf2_hmac_sha256("password", "salt", 4096, 32, dk));
    CHECK(hex_encode(dk) ==
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    CHECK(!pbkdf2_hmac_sha256("password", "salt", 0, 32, dk) && dk.empty());
    CHECK(!pbkdf2_hmac_sha256("password", "salt", 10, 0, dk) && dk.empty());
}

void test_aes() {
    using namespace myframe::crypto;

    // NIST SP 800-38D AES-128-GCM Test Case 1 (empty plaintext & AAD)
    {
        std::string ct, tag, pt;
        std::string k(16, '\0'), iv(12, '\0');
        CHECK(aes_gcm_encrypt(k, iv, "", ct, tag));
        CHECK(ct.empty() && hex_encode(tag) == "58e2fccefa7e3061367f1d57a4e7455a");
        CHECK(aes_gcm_decrypt(k, iv, ct, tag, pt) && pt.empty());
    }

    // NIST SP 800-38D AES-128-GCM Test Case 4 (with AAD)
    {
        std::string k   = from_hex("feffe9928665731c6d6a8f9467308308");
        std::string iv  = from_hex("cafebabefacedbaddecaf888");
        std::string pt0 = from_hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                                   "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        std::string aad = from_hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        std::string ct, tag, pt;
        CHECK(aes_gcm_encrypt(k, iv, pt0, ct, tag, aad));
        CHECK(hex_encode(ct) ==
              "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
              "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
        CHECK(hex_encode(tag) == "5bc94fbc3221a5db94fae95ae7121a47");
        CHECK(aes_gcm_decrypt(k, iv, ct, tag, pt, aad) && pt == pt0);
        // Tampered tag, ciphertext, or AAD must fail and clear output.
        std::string bad_tag = tag; bad_tag[0] ^= 1;
        CHECK(!aes_gcm_decrypt(k, iv, ct, bad_tag, pt, aad) && pt.empty());
        std::string bad_ct = ct; bad_ct[5] ^= 0x80;
        CHECK(!aes_gcm_decrypt(k, iv, bad_ct, tag, pt, aad) && pt.empty());
        CHECK(!aes_gcm_decrypt(k, iv, ct, tag, pt, aad + "x") && pt.empty());
    }

    // NIST SP 800-38D AES-256-GCM Test Case 16 (with AAD)
    {
        std::string k   = from_hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        std::string iv  = from_hex("cafebabefacedbaddecaf888");
        std::string pt0 = from_hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                                   "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        std::string aad = from_hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        std::string ct, tag, pt;
        CHECK(aes_gcm_encrypt(k, iv, pt0, ct, tag, aad));
        CHECK(hex_encode(ct) ==
              "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
              "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
        CHECK(hex_encode(tag) == "76fc6ece0f4e1768cddf8853bb2d551b");
        CHECK(aes_gcm_decrypt(k, iv, ct, tag, pt, aad) && pt == pt0);
    }

    // High-level self-contained seal/open for 128/192/256-bit keys + Base64 variant
    const std::size_t key_lens[] = {16, 24, 32};
    for (std::size_t i = 0; i < 3; ++i) {
        std::string key;
        CHECK(random_bytes(key_lens[i], key));
        std::string s1, s2, out;
        CHECK(aes_gcm_seal(key, "hello aes-gcm", s1, "tenant-42"));
        CHECK(aes_gcm_seal(key, "hello aes-gcm", s2, "tenant-42"));
        CHECK(s1 != s2); // fresh random nonce every time
        CHECK(aes_gcm_open(key, s1, out, "tenant-42") && out == "hello aes-gcm");
        CHECK(aes_gcm_open(key, s2, out, "tenant-42") && out == "hello aes-gcm");
        CHECK(!aes_gcm_open(key, s1, out, "wrong-aad") && out.empty());
        s1[s1.size() - 1] ^= 0x01;
        CHECK(!aes_gcm_open(key, s1, out, "tenant-42") && out.empty());

        std::string b64;
        CHECK(aes_gcm_seal_base64(key, "", b64, "empty-pt"));
        CHECK(aes_gcm_open_base64(key, b64, out, "empty-pt") && out.empty());
    }

    // Reject bad key lengths & truncated sealed payloads + two-output aliasing audit
    {
        std::string out, err;
        CHECK(!aes_gcm_seal("short-key", "hi", out, "", &err) && out.empty() && !err.empty());
        CHECK(!aes_gcm_open(std::string(32, 'K'), "too-short", out, "", &err) && out.empty());

        std::string key(32, 'K'), nonce(12, 'N');
        std::string pt = "explicit aes-gcm aliasing payload";
        std::string aad = "explicit-aad";
        std::string exp_ct, exp_tag;
        CHECK(aes_gcm_encrypt(key, nonce, pt, exp_ct, exp_tag, aad, &err));

        // Identical output references (&out_ciphertext == &out_tag) MUST fail explicitly
        // and clear the output rather than succeeding with lost ciphertext.
        std::string shared = "stale-output";
        err.clear();
        CHECK(!aes_gcm_encrypt(key, nonce, pt, shared, shared, aad, &err));
        CHECK(shared.empty() && !err.empty());

        // Failure with distinct pre-filled outputs clears both outputs.
        std::string stale_ct = "stale-ct", stale_tag = "stale-tag";
        CHECK(!aes_gcm_encrypt("bad-key", nonce, pt, stale_ct, stale_tag, aad, &err));
        CHECK(stale_ct.empty() && stale_tag.empty());

        // Input-output aliasing with distinct outputs remains supported.
        std::string buf = pt, tag_out;
        CHECK(aes_gcm_encrypt(key, nonce, buf, buf, tag_out, aad, &err));
        CHECK(buf == exp_ct && tag_out == exp_tag);

        std::string ct_out;
        buf = pt;
        CHECK(aes_gcm_encrypt(key, nonce, buf, ct_out, buf, aad, &err));
        CHECK(ct_out == exp_ct && buf == exp_tag);

        std::string k_alias = key, n_alias = nonce;
        CHECK(aes_gcm_encrypt(k_alias, n_alias, pt, k_alias, n_alias, aad, &err));
        CHECK(k_alias == exp_ct && n_alias == exp_tag);

        std::string aad_alias = aad;
        tag_out.clear();
        CHECK(aes_gcm_encrypt(key, nonce, pt, aad_alias, tag_out, aad_alias, &err));
        CHECK(aad_alias == exp_ct && tag_out == exp_tag);

        // Decrypt input-output aliasing (&out_plaintext == &ciphertext / &tag / &key).
        std::string dec = exp_ct;
        CHECK(aes_gcm_decrypt(key, nonce, dec, exp_tag, dec, aad, &err) && dec == pt);
        dec = exp_tag;
        CHECK(aes_gcm_decrypt(key, nonce, exp_ct, dec, dec, aad, &err) && dec == pt);
        dec = key;
        CHECK(aes_gcm_decrypt(dec, nonce, exp_ct, exp_tag, dec, aad, &err) && dec == pt);
    }

    // AES-CBC (PKCS#7) round-trip + bad padding rejection + large 1 MiB payload
    {
        std::string key(32, 'K'), iv(16, 'I');
        std::string pt = "0123456789abcdef"; // exact 16-byte multiple (adds full pad block)
        std::string ct, out;
        CHECK(aes_cbc_encrypt(key, iv, pt, ct) && ct.size() == 32);
        CHECK(aes_cbc_decrypt(key, iv, ct, out) && out == pt);
        // Empty plaintext -> 16-byte padded block
        CHECK(aes_cbc_encrypt(key, iv, "", ct) && ct.size() == 16);
        CHECK(aes_cbc_decrypt(key, iv, ct, out) && out.empty());
        // Corrupt ciphertext -> bad padding failure
        std::string bad = ct; bad[15] ^= 0xFF;
        CHECK(!aes_cbc_decrypt(key, iv, bad, out) && out.empty());
        // 1 MiB payload + in-place aliasing (&out == &in) across CBC/GCM/CTR/codec
        std::string big(1024 * 1024 + 13, 'Z');
        CHECK(aes_cbc_encrypt(key, iv, big, ct));
        CHECK(aes_cbc_decrypt(key, iv, ct, out) && out == big);
        std::string alias = big;
        CHECK(aes_cbc_encrypt(key, iv, alias, alias));
        CHECK(aes_cbc_decrypt(key, iv, alias, alias) && alias == big);
        std::string gcm_alias = "in-place gcm test";
        CHECK(aes_gcm_seal(key, gcm_alias, gcm_alias, "aad"));
        CHECK(aes_gcm_open(key, gcm_alias, gcm_alias, "aad") && gcm_alias == "in-place gcm test");
        std::string ctr_alias = "in-place ctr test";
        CHECK(aes_ctr_crypt(key, iv, ctr_alias, ctr_alias));
        CHECK(aes_ctr_crypt(key, iv, ctr_alias, ctr_alias) && ctr_alias == "in-place ctr test");
        std::string b64_alias = base64_encode(std::string("in-place b64"));
        CHECK(base64_decode(b64_alias, b64_alias) && b64_alias == "in-place b64");
        std::string hex_alias = hex_encode(std::string("in-place hex"));
        CHECK(hex_decode(hex_alias, hex_alias) && hex_alias == "in-place hex");
    }

    // AES-CTR NIST SP 800-38A F.5.1 (AES-128-CTR) + symmetry
    {
        std::string k  = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
        std::string iv = from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        std::string pt = from_hex("6bc1bee22e409f96e93d7e117393172a");
        std::string ct, dec;
        CHECK(aes_ctr_crypt(k, iv, pt, ct));
        CHECK(hex_encode(ct) == "874d6191b620e3261bef6864990db6ce");
        CHECK(aes_ctr_crypt(k, iv, ct, dec) && dec == pt);
        CHECK(aes_ctr_crypt(k, iv, "", ct) && ct.empty());
    }
}

void test_asym() {
    using namespace myframe::crypto;

    // 0) Identical output references (&out_private_pem == &out_public_pem) must fail
    //    explicitly and clear the output rather than returning success with lost private key.
    {
        std::string shared = "stale-pem", err;
        CHECK(!generate_rsa_keypair(2048, shared, shared, &err));
        CHECK(shared.empty() && !err.empty());

        shared = "stale-pem";
        err.clear();
        CHECK(!generate_ec_p256_keypair(shared, shared, &err));
        CHECK(shared.empty() && !err.empty());

        shared = "stale-pem";
        err.clear();
        CHECK(!generate_ed25519_keypair(shared, shared, &err));
        CHECK(shared.empty() && !err.empty());
    }

    std::string rsa_priv, rsa_pub;
    std::string ec_priv, ec_pub;
    std::string ed_priv, ed_pub;

    // 1) RSA-2048 OAEP-SHA256 + PSS-SHA256 (normal, in-place aliasing, failure clearing)
    {
        std::string err;
        std::string stale_priv = "stale-priv", stale_pub = "stale-pub";
        CHECK(!generate_rsa_keypair(1024, stale_priv, stale_pub, &err)); // rejects weak key < 2048
        CHECK(stale_priv.empty() && stale_pub.empty() && !err.empty());

        CHECK(generate_rsa_keypair(2048, rsa_priv, rsa_pub, &err));
        CHECK(rsa_priv.find("BEGIN PRIVATE KEY") != std::string::npos);
        CHECK(rsa_pub.find("BEGIN PUBLIC KEY") != std::string::npos);

        // RSA-OAEP normal + tamper + in-place aliasing + empty plaintext
        std::string ct, pt;
        CHECK(rsa_oaep_encrypt(rsa_pub, "top secret session key", ct, &err));
        CHECK(rsa_oaep_decrypt(rsa_priv, ct, pt, &err) && pt == "top secret session key");
        std::string oaep_alias = "top secret session key";
        CHECK(rsa_oaep_encrypt(rsa_pub, oaep_alias, oaep_alias, &err));
        CHECK(rsa_oaep_decrypt(rsa_priv, oaep_alias, oaep_alias, &err) &&
              oaep_alias == "top secret session key");
        CHECK(rsa_oaep_encrypt(rsa_pub, "", ct, &err));
        CHECK(rsa_oaep_decrypt(rsa_priv, ct, pt, &err) && pt.empty());
        ct[0] ^= 0xFF;
        pt = "stale-pt";
        CHECK(!rsa_oaep_decrypt(rsa_priv, ct, pt, &err) && pt.empty());

        // RSA-PSS normal vs in-place (&out_sig == &message and &out_sig == &private_pem)
        const std::string msg = "signed rsa-pss payload";
        std::string sig;
        CHECK(rsa_pss_sign(rsa_priv, msg, sig, &err));
        CHECK(rsa_pss_verify(rsa_pub, msg, sig, &err));
        CHECK(!rsa_pss_verify(rsa_pub, "tampered payload", sig, &err));
        CHECK(!rsa_pss_verify(rsa_pub, "", sig, &err));
        sig[0] ^= 0x01;
        CHECK(!rsa_pss_verify(rsa_pub, msg, sig, &err));

        std::string inplace_sig = msg;
        CHECK(rsa_pss_sign(rsa_priv, inplace_sig, inplace_sig, &err));
        CHECK(rsa_pss_verify(rsa_pub, msg, inplace_sig, &err));
        CHECK(!rsa_pss_verify(rsa_pub, "", inplace_sig, &err));

        std::string key_alias = rsa_priv;
        CHECK(rsa_pss_sign(key_alias, msg, key_alias, &err));
        CHECK(rsa_pss_verify(rsa_pub, msg, key_alias, &err));
        CHECK(!rsa_pss_verify(rsa_pub, "", key_alias, &err));

        // Failure clears pre-filled out_sig
        std::string fail_sig = "stale-rsa-sig";
        err.clear();
        CHECK(!rsa_pss_sign("not-a-valid-pem", msg, fail_sig, &err));
        CHECK(fail_sig.empty() && !err.empty());
    }

    // 2) ECDSA P-256 (SHA-256) (normal, in-place aliasing, failure clearing)
    {
        std::string sig, err;
        CHECK(generate_ec_p256_keypair(ec_priv, ec_pub, &err));

        const std::string msg = "ecdsa message";
        CHECK(ecdsa_sha256_sign(ec_priv, msg, sig, &err));
        CHECK(ecdsa_sha256_verify(ec_pub, msg, sig, &err));
        CHECK(!ecdsa_sha256_verify(ec_pub, "ecdsa message!", sig, &err));
        CHECK(!ecdsa_sha256_verify(ec_pub, "", sig, &err));

        std::string inplace_sig = msg;
        CHECK(ecdsa_sha256_sign(ec_priv, inplace_sig, inplace_sig, &err));
        CHECK(ecdsa_sha256_verify(ec_pub, msg, inplace_sig, &err));
        CHECK(!ecdsa_sha256_verify(ec_pub, "", inplace_sig, &err));

        std::string key_alias = ec_priv;
        CHECK(ecdsa_sha256_sign(key_alias, msg, key_alias, &err));
        CHECK(ecdsa_sha256_verify(ec_pub, msg, key_alias, &err));
        CHECK(!ecdsa_sha256_verify(ec_pub, "", key_alias, &err));

        // Wrong key type rejected and clears pre-filled outputs
        std::string ct = "stale-ct";
        CHECK(!rsa_oaep_encrypt(ec_pub, "x", ct, &err) && ct.empty());
        std::string fail_sig = "stale-ec-sig";
        CHECK(!ecdsa_sha256_sign(rsa_priv, msg, fail_sig, &err) && fail_sig.empty());
        fail_sig = "stale-ec-sig";
        CHECK(!ecdsa_sha256_sign("bad-pem", msg, fail_sig, &err) && fail_sig.empty());
        fail_sig = "stale-rsa-sig";
        CHECK(!rsa_pss_sign(ec_priv, msg, fail_sig, &err) && fail_sig.empty());
    }

    // 3) Ed25519 (normal, in-place aliasing, determinism, failure clearing)
    {
        std::string sig, err;
        CHECK(generate_ed25519_keypair(ed_priv, ed_pub, &err));

        const std::string msg = "ed25519 message";
        CHECK(ed25519_sign(ed_priv, msg, sig, &err));
        CHECK(sig.size() == 64u);
        CHECK(ed25519_verify(ed_pub, msg, sig, &err));
        CHECK(!ed25519_verify(ed_pub, "ed25519 message!", sig, &err));
        CHECK(!ed25519_verify(ed_pub, "", sig, &err));

        // In-place &out_sig == &message produces the exact same deterministic Ed25519 signature
        std::string inplace_sig = msg;
        CHECK(ed25519_sign(ed_priv, inplace_sig, inplace_sig, &err));
        CHECK(inplace_sig == sig);
        CHECK(ed25519_verify(ed_pub, msg, inplace_sig, &err));
        CHECK(!ed25519_verify(ed_pub, "", inplace_sig, &err));

        // In-place &out_sig == &private_pem
        std::string key_alias = ed_priv;
        CHECK(ed25519_sign(key_alias, msg, key_alias, &err));
        CHECK(key_alias == sig);
        CHECK(ed25519_verify(ed_pub, msg, key_alias, &err));

        sig[63] ^= 0x80;
        CHECK(!ed25519_verify(ed_pub, msg, sig, &err));

        // Failure clears pre-filled out_sig
        std::string fail_sig = "stale-ed-sig";
        CHECK(!ed25519_sign(ec_priv, msg, fail_sig, &err) && fail_sig.empty());
        fail_sig = "stale-ed-sig";
        CHECK(!ed25519_sign("bad-pem", msg, fail_sig, &err) && fail_sig.empty());

        // Empty message supported (including in-place empty string)
        CHECK(ed25519_sign(ed_priv, "", sig, &err));
        CHECK(ed25519_verify(ed_pub, "", sig, &err));
    }
}

void test_concurrent_safety() {
    using namespace myframe::crypto;
    // Exercise AES-GCM, SHA-256, HMAC, HKDF, Ed25519 across 8 threads (for TSan).
    std::string ed_priv, ed_pub;
    CHECK(generate_ed25519_keypair(ed_priv, ed_pub));
    std::string key(32, 'T');

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 40; ++i) {
                std::string msg = "thread-" + std::to_string(t) + "-iter-" + std::to_string(i);
                std::string sealed, plain, dk, sig;
                CHECK(aes_gcm_seal(key, msg, sealed, "aad"));
                CHECK(aes_gcm_open(key, sealed, plain, "aad") && plain == msg);
                CHECK(hmac_sha256_verify_hex(key, msg, hmac_sha256_hex(key, msg)));
                CHECK(hkdf_sha256(key, "salt", msg, 32, dk) && dk.size() == 32);
                CHECK(ed25519_sign(ed_priv, msg, sig) && ed25519_verify(ed_pub, msg, sig));
            }
        });
    }
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
}

} // namespace

int main() {
    test_random_and_codec();
    test_hash_and_hmac();
    test_kdf();
    test_aes();
    test_asym();
    test_concurrent_safety();

    if (g_fails.load() == 0) {
        std::printf("test_crypto: ALL PASS (%d checks)\n", g_checks.load());
        return 0;
    }
    std::printf("test_crypto: %d of %d checks FAILED\n", g_fails.load(), g_checks.load());
    return 1;
}
