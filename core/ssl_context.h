#pragma once

#include "base_def.h"
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cstring>


// forward decl for ALPN callback context cast
class ssl_context;

// forward declaration of ALPN callback
static int myframe_alpn_select_cb(SSL*, const unsigned char**, unsigned char*, const unsigned char*, unsigned int, void*);

struct ssl_config {
    std::string _cert_file;
    std::string _key_file;
    std::string _protocols; // e.g., "TLSv1.2,TLSv1.3"
    bool _verify_peer = false;
    std::string _ca_file; // optional CA bundle for client verification
    std::string _cipher_list; // optional cipher list
    // Optional session settings
    bool _enable_session_cache{true};
    long _session_cache_size{4096};
    bool _enable_tickets{true};
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
            if (SSL_CTX_check_private_key(_ctx) != 1) { PDEBUG("%s", "[tls] Private key does not match certificate"); ERR_print_errors_fp(stderr); return false; }
        }
        if (!conf._cert_file.empty() || !conf._key_file.empty()) {
            PDEBUG("[tls] Using cert='%s' key='%s'", conf._cert_file.c_str(), conf._key_file.c_str());
        }
        if (!conf._cipher_list.empty()) {
            if (SSL_CTX_set_cipher_list(_ctx, conf._cipher_list.c_str()) != 1) { ERR_print_errors_fp(stderr); return false; }
        }
        // Session cache and tickets (reduce handshake cost)
        {
            bool enable_cache = conf._enable_session_cache; long cache_sz = conf._session_cache_size;
            bool enable_tickets = conf._enable_tickets;
            // Env overrides: MYFRAME_SSL_SESS_CACHE(1/0), MYFRAME_SSL_SESS_CACHE_SIZE, MYFRAME_SSL_TICKETS(1/0)
            if (const char* e = ::getenv("MYFRAME_SSL_SESS_CACHE")) {
                enable_cache = (strcmp(e, "0") != 0 && strcasecmp(e, "false") != 0);
            }
            if (const char* e = ::getenv("MYFRAME_SSL_SESS_CACHE_SIZE")) {
                long v = atol(e); if (v > 0) cache_sz = v;
            }
            if (const char* e = ::getenv("MYFRAME_SSL_TICKETS")) {
                enable_tickets = (strcmp(e, "0") != 0 && strcasecmp(e, "false") != 0);
            }
            if (enable_cache) {
                SSL_CTX_set_session_cache_mode(_ctx, SSL_SESS_CACHE_SERVER);
                SSL_CTX_sess_set_cache_size(_ctx, cache_sz);
            } else {
                SSL_CTX_set_session_cache_mode(_ctx, SSL_SESS_CACHE_OFF);
            }
#ifdef SSL_OP_NO_TICKET
            if (!enable_tickets) {
                SSL_CTX_set_options(_ctx, SSL_OP_NO_TICKET);
            } else {
                // Ensure tickets not disabled
                SSL_CTX_clear_options(_ctx, SSL_OP_NO_TICKET);
            }
#endif
        }
        // 启用 ALPN，优先 h2 回退 http/1.1；可由环境限制
        _allow_h2 = true; _allow_h11 = true;
        // 从环境变量覆盖：MYFRAME_SSL_ALPN (例如: "h2,http/1.1" 或 "http/1.1")
        const char* env_alpn = ::getenv("MYFRAME_SSL_ALPN");
        if (env_alpn && *env_alpn) {
            std::string s(env_alpn); for (auto& c : s) c = (char)tolower(c);
            _allow_h2  = (s.find("h2") != std::string::npos);
            _allow_h11 = (s.find("http/1.1") != std::string::npos) || (s.find("http1.1") != std::string::npos);
        }
        SSL_CTX_set_alpn_select_cb(_ctx, myframe_alpn_select_cb, this);
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
public:
    bool _allow_h2{true};
    bool _allow_h11{true};
};

struct ssl_context_singleton {
    static ssl_context* get_instance_ex() { static ssl_context g; return &g; }
};

// ALPN选择回调：优先选择 h2，否则回退 http/1.1（允许受 ssl_context 配置限制）
static inline int myframe_alpn_select_cb(SSL* /*ssl*/, const unsigned char** out,
                                         unsigned char* outlen,
                                         const unsigned char* in,
                                         unsigned int inlen,
                                         void* arg)
{
    static const unsigned char H2[]     = "h2";          // 不包含长度前缀
    static const unsigned char HTTP11[] = "http/1.1";    // 不包含长度前缀
    bool allow_h2 = true, allow_h11 = true;
    if (arg) {
        ssl_context* self = reinterpret_cast<ssl_context*>(arg);
        allow_h2  = self->_allow_h2;
        allow_h11 = self->_allow_h11;
    }

    auto find_proto = [&](const unsigned char* want, unsigned int want_len) -> bool {
#ifdef DEBUG
        // Debug print offered protocols
        PDEBUG("[alpn] offered list (%u bytes)", inlen);
        for (unsigned int i = 0; i < inlen;) {
            unsigned int l = in[i++];
            if (i + l > inlen) break;
            PDEBUG("[alpn] offered '%.*s'", (int)l, &in[i]);
            i += l;
        }
#endif
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

    if (allow_h2 && find_proto(H2, (unsigned int)(sizeof(H2) - 1))) {
        *out = H2; *outlen = (unsigned char)(sizeof(H2) - 1); return SSL_TLSEXT_ERR_OK;
    }
    if (allow_h11 && find_proto(HTTP11, (unsigned int)(sizeof(HTTP11) - 1))) {
        *out = HTTP11; *outlen = (unsigned char)(sizeof(HTTP11) - 1); return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}
