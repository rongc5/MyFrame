#pragma once

#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "log_helper.h"

#if defined(SSL_CTX_set_alpn_select_cb)
// ALPN选择回调：优先选择 h2，否则回退 http/1.1
static inline int myframe_alpn_select_cb(SSL* /*ssl*/, const unsigned char** out,
                                         unsigned char* outlen,
                                         const unsigned char* in,
                                         unsigned int inlen,
                                         void* /*arg*/)
{
    static const unsigned char H2[]     = "h2";          // 不包含长度前缀
    static const unsigned char HTTP11[] = "http/1.1";    // 不包含长度前缀

    auto find_proto = [&](const unsigned char* want, unsigned int want_len) -> bool {
        for (unsigned int i = 0; i < inlen;) {
            unsigned int l = in[i++];
            if (i + l > inlen) break;
            if (l == want_len && std::memcmp(&in[i], want, l) == 0) {
                return true;
            }
            i += l;
        }
        return false;
    };

    if (find_proto(H2, (unsigned int)(sizeof(H2) - 1))) {
        *out = H2; *outlen = (unsigned char)(sizeof(H2) - 1); return SSL_TLSEXT_ERR_OK;
    }
    if (find_proto(HTTP11, (unsigned int)(sizeof(HTTP11) - 1))) {
        *out = HTTP11; *outlen = (unsigned char)(sizeof(HTTP11) - 1); return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}
#endif

struct ssl_config {
    std::string _cert_file;
    std::string _key_file;
    std::string _protocols; // e.g., "TLSv1.2,TLSv1.3"
    bool _verify_peer = false;
    std::string _ca_file; // optional CA bundle for client verification
    std::string _cipher_list; // optional cipher list
};

void tls_set_server_config(const ssl_config& conf);
bool tls_get_server_config(ssl_config& out);
void tls_set_client_config(const ssl_config& conf);
bool tls_get_client_config(ssl_config& out);
// Reset runtime TLS state (server/client configs). See tls_runtime.cpp
void tls_reset_runtime();

class ssl_context {
public:
    ssl_context() : _ctx(0), _inited(false) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    }
    ~ssl_context() { if (_ctx) SSL_CTX_free(_ctx); }

    bool is_initialized() const { return _inited; }

    // Cleanup global SSL context and allow re-init.
    // Returns true if context was cleaned or not initialized.
    bool cleanup_global() {
        if (_ctx) {
            SSL_CTX_free(_ctx);
            _ctx = nullptr;
        }
        _inited = false;
        // reset runtime configs so next init can reload certs/keys
        tls_reset_runtime();
        return true;
    }

    bool init_server(const ssl_config& conf) {
        if (_inited) return true;
        const SSL_METHOD* method = TLS_server_method();
        _ctx = SSL_CTX_new(method);
        if (!_ctx) { ERR_print_errors_fp(stderr); return false; }
        if (!conf._cert_file.empty()) {
            if (SSL_CTX_use_certificate_file(_ctx, conf._cert_file.c_str(), SSL_FILETYPE_PEM) != 1) { ERR_print_errors_fp(stderr); return false; }
        }
        if (!conf._key_file.empty()) {
            if (SSL_CTX_use_PrivateKey_file(_ctx, conf._key_file.c_str(), SSL_FILETYPE_PEM) != 1) { ERR_print_errors_fp(stderr); return false; }
            if (SSL_CTX_check_private_key(_ctx) != 1) { std::fprintf(stderr, "[tls] Private key does not match certificate\n"); ERR_print_errors_fp(stderr); return false; }
        }
        if (!conf._cert_file.empty() || !conf._key_file.empty()) {
            LOG_NOTICE("[tls] Using cert='%s' key='%s'", conf._cert_file.c_str(), conf._key_file.c_str());
        }
        if (!conf._cipher_list.empty()) {
            if (SSL_CTX_set_cipher_list(_ctx, conf._cipher_list.c_str()) != 1) { ERR_print_errors_fp(stderr); return false; }
        }
        // 启用 ALPN，优先 h2 回退 http/1.1
#if defined(SSL_CTX_set_alpn_select_cb)
        SSL_CTX_set_alpn_select_cb(_ctx, myframe_alpn_select_cb, nullptr);
#endif
        // Enforce protocol range if provided (expects CSV like "TLSv1.2,TLSv1.3")
#if defined(TLS1_VERSION)
        if (!conf._protocols.empty()) {
            auto has = [&](const char* t) { return conf._protocols.find(t) != std::string::npos; };
            long minv = 0, maxv = 0;
#if defined(TLS1_3_VERSION)
            if (has("TLSv1.3")) maxv = TLS1_3_VERSION;
#endif
#if defined(TLS1_2_VERSION)
            if (maxv == 0 && has("TLSv1.2")) maxv = TLS1_2_VERSION;
            if (has("TLSv1.2")) minv = (minv == 0 ? TLS1_2_VERSION : minv);
#endif
#if defined(TLS1_1_VERSION)
            if (has("TLSv1.1")) minv = (minv == 0 ? TLS1_1_VERSION : minv);
#endif
#if defined(TLS1_VERSION)
            if (has("TLSv1"))   minv = (minv == 0 ? TLS1_VERSION : minv);
#endif
#if defined(SSL_CTX_set_min_proto_version)
            if (minv) SSL_CTX_set_min_proto_version(_ctx, (int)minv);
            if (maxv) SSL_CTX_set_max_proto_version(_ctx, (int)maxv);
#else
            // Fallback: disable older protocols via options if min >= TLS1_2
            if (minv >= TLS1_2_VERSION) {
                SSL_CTX_set_options(_ctx, SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
            }
#endif
        } else {
            // Safe defaults: TLS1.2+
#if defined(SSL_CTX_set_min_proto_version) && defined(TLS1_2_VERSION)
            SSL_CTX_set_min_proto_version(_ctx, TLS1_2_VERSION);
#else
            SSL_CTX_set_options(_ctx, SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#endif
        }
#endif
        // Optional peer verification
        if (conf._verify_peer) {
            SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            if (!conf._ca_file.empty()) {
                if (SSL_CTX_load_verify_locations(_ctx, conf._ca_file.c_str(), nullptr) != 1) { ERR_print_errors_fp(stderr); return false; }
            }
        } else {
            SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);
        }
        _inited = true; return true;
    }

    SSL* create_ssl() { if (!_ctx) return 0; return SSL_new(_ctx); }

private:
    SSL_CTX* _ctx; bool _inited;
};

struct ssl_context_singleton {
    static ssl_context* get_instance_ex() { static ssl_context g; return &g; }
};
