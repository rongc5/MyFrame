#pragma once

#include "out_connect.h"
#include "codec.h"
#include "client_ssl_codec.h"
#include "ssl_context.h"
#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

// tls_out_connect: wrap out_connect to install a TLS client codec on connect.
// Usage: std::make_shared< tls_out_connect<http_req_process> >(host, port, server_name)
template<class PROCESS>
class tls_out_connect : public out_connect<PROCESS> {
public:
    tls_out_connect(const std::string& ip, unsigned short port, const std::string& host, const std::string& alpn = std::string())
        : out_connect<PROCESS>(ip, port), _host(host), _alpn(alpn)
#ifdef ENABLE_SSL
        , _ssl(nullptr)
#endif
    {
#ifdef ENABLE_SSL
        // Ensure shared client SSL_CTX is initialized (thread-safe, idempotent).
        // Always call init_client() — it returns immediately if already initialized.
        ssl_context* ctx_inst = ssl_client_context_singleton::get_instance_ex();
        ssl_config conf;
        if (tls_get_client_config(conf)) {
            if (!ctx_inst->init_client(conf)) {
                THROW_COMMON_EXCEPT("[tls] FATAL: client SSL_CTX init failed (runtime config)");
            }
        } else {
            // Env fallbacks for quick testing
            ssl_config env_conf;
            env_conf._verify_peer = true;
            if (const char* env_verify = ::getenv("MYFRAME_SSL_VERIFY")) {
                if (strcmp(env_verify, "0") == 0 || strcasecmp(env_verify, "false") == 0) {
                    env_conf._verify_peer = false;
                }
            }
            if (const char* env_ca = ::getenv("MYFRAME_SSL_CA")) {
                if (*env_ca) env_conf._ca_file = env_ca;
            }
            const char* env_cert = ::getenv("MYFRAME_SSL_CLIENT_CERT");
            const char* env_key  = ::getenv("MYFRAME_SSL_CLIENT_KEY");
            if (env_cert && *env_cert) env_conf._cert_file = env_cert;
            if (env_key  && *env_key ) env_conf._key_file = env_key;
            if (!ctx_inst->init_client(env_conf)) {
                THROW_COMMON_EXCEPT("[tls] FATAL: client SSL_CTX init failed (env config)");
            }
        }
#endif
    }

    virtual ~tls_out_connect() {
#ifdef ENABLE_SSL
        if (_ssl) { SSL_free(_ssl); _ssl = nullptr; }
        // SSL_CTX is owned by the singleton — do NOT free here
#endif
    }

protected:
    virtual void connect_ok_process() override {
        out_connect<PROCESS>::connect_ok_process();
#ifdef ENABLE_SSL
        SSL_CTX* ssl_ctx = ssl_client_context_singleton::get_instance_ex()->get_ctx();
        if (!ssl_ctx) {
            THROW_COMMON_EXCEPT("[tls] FATAL: SSL_CTX is null in connect_ok_process");
        }
        _ssl = SSL_new(ssl_ctx);
        if (!_ssl) {
            ERR_print_errors_fp(stderr);
            THROW_COMMON_EXCEPT("[tls] FATAL: SSL_new() returned null");
        }
        // Store hostname in ex_data for session cache callback
        SSL_set_ex_data(_ssl, SslSessionCache::host_index(), strdup(_host.c_str()));
        // Try to reuse cached TLS session (avoids full handshake)
        SSL_SESSION* cached_sess = SslSessionCache::instance().get(_host);
        if (cached_sess) {
            SSL_set_session(_ssl, cached_sess);
            SSL_SESSION_free(cached_sess);
        }
        SSL_set_tlsext_host_name(_ssl, _host.c_str());
        if (!_host.empty()) {
            X509_VERIFY_PARAM* param = SSL_get0_param(_ssl);
            // Enforce exact hostname match; SANs are preferred but CN fallback applies.
            X509_VERIFY_PARAM_set1_host(param, _host.c_str(), 0);
        }
        SSL_set_fd(_ssl, this->_fd);
        if (!_alpn.empty()) {
            // Build ALPN wire format: length-prefixed protocol names
            std::string list;
            size_t start = 0;
            while (start <= _alpn.size()) {
                size_t comma = _alpn.find(',', start);
                std::string proto = _alpn.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!proto.empty()) {
                    if (proto.size() > 255) proto.resize(255);
                    list.push_back((char)proto.size()); list.append(proto);
                }
                if (comma == std::string::npos) break; start = comma + 1;
            }
            if (!list.empty()) {
                SSL_set_alpn_protos(_ssl, (const unsigned char*)list.data(), (unsigned int)list.size());
            }
        }
        // Transfer ownership of SSL* to codec to avoid double free in connector dtor
        this->set_codec(std::unique_ptr<ICodec>(new ClientSslCodec(_ssl)));
        _ssl = nullptr;
        // Ensure EPOLLOUT to drive SSL_connect progress
        this->update_event(this->get_event() | EPOLLOUT);
#endif
    }

private:
    std::string _host;
    std::string _alpn;
#ifdef ENABLE_SSL
    SSL* _ssl;
#endif
};
