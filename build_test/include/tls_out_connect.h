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
        , _ssl_ctx(nullptr), _ssl(nullptr)
#endif
    {
#ifdef ENABLE_SSL
        SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
        _ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (_ssl_ctx) {
            // Apply runtime client TLS config if provided
            ssl_config conf; bool has = tls_get_client_config(conf);
            if (has) {
                if (!conf._cipher_list.empty()) {
                    SSL_CTX_set_cipher_list(_ssl_ctx, conf._cipher_list.c_str());
                }
                if (conf._verify_peer) {
                    SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_PEER, nullptr);
                    if (!conf._ca_file.empty()) {
                        SSL_CTX_load_verify_locations(_ssl_ctx, conf._ca_file.c_str(), nullptr);
                    } else if (SSL_CTX_set_default_verify_paths(_ssl_ctx) != 1) {
                        PDEBUG("[tls] WARNING: failed to load default verify paths");
                    }
                } else {
                    SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_NONE, nullptr);
                }
                // Optional mTLS: reuse _cert_file/_key_file fields
                if (!conf._cert_file.empty()) {
                    SSL_CTX_use_certificate_file(_ssl_ctx, conf._cert_file.c_str(), SSL_FILETYPE_PEM);
                }
                if (!conf._key_file.empty()) {
                    SSL_CTX_use_PrivateKey_file(_ssl_ctx, conf._key_file.c_str(), SSL_FILETYPE_PEM);
                }
            } else {
                // Env fallbacks for quick testing
                bool verify_peer = true;
                if (const char* env_verify = ::getenv("MYFRAME_SSL_VERIFY")) {
                    if (strcmp(env_verify, "0") == 0 || strcasecmp(env_verify, "false") == 0) {
                        verify_peer = false;
                    }
                }
                if (verify_peer) {
                    SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_PEER, nullptr);
                    const char* env_ca = ::getenv("MYFRAME_SSL_CA");
                    bool loaded = false;
                    if (env_ca && *env_ca) {
                        if (SSL_CTX_load_verify_locations(_ssl_ctx, env_ca, nullptr) == 1) {
                            loaded = true;
                        } else {
                            PDEBUG("[tls] WARNING: failed to load CA bundle from %s", env_ca);
                        }
                    }
                    if (!loaded && SSL_CTX_set_default_verify_paths(_ssl_ctx) != 1) {
                        PDEBUG("[tls] WARNING: default CA paths unavailable; TLS verification may fail");
                    }
                } else {
                    SSL_CTX_set_verify(_ssl_ctx, SSL_VERIFY_NONE, nullptr);
                    PDEBUG("[tls] WARNING: peer verification disabled (MYFRAME_SSL_VERIFY=0)");
                }
                const char* env_cert = ::getenv("MYFRAME_SSL_CLIENT_CERT");
                const char* env_key  = ::getenv("MYFRAME_SSL_CLIENT_KEY");
                if (env_cert && *env_cert) SSL_CTX_use_certificate_file(_ssl_ctx, env_cert, SSL_FILETYPE_PEM);
                if (env_key  && *env_key ) SSL_CTX_use_PrivateKey_file(_ssl_ctx, env_key,  SSL_FILETYPE_PEM);
            }
        }
#endif
    }

    virtual ~tls_out_connect() {
#ifdef ENABLE_SSL
        if (_ssl) { SSL_free(_ssl); _ssl = nullptr; }
        if (_ssl_ctx) { SSL_CTX_free(_ssl_ctx); _ssl_ctx = nullptr; }
#endif
    }

protected:
    virtual void connect_ok_process() override {
        out_connect<PROCESS>::connect_ok_process();
#ifdef ENABLE_SSL
        if (!_ssl_ctx) return;
        // Enable client session cache if configured (useful when sharing SSL_CTX across conns)
        ssl_config ctmp; bool has = tls_get_client_config(ctmp);
        if (has) {
            if (ctmp._enable_session_cache) {
                SSL_CTX_set_session_cache_mode(_ssl_ctx, SSL_SESS_CACHE_CLIENT);
            } else {
                SSL_CTX_set_session_cache_mode(_ssl_ctx, SSL_SESS_CACHE_OFF);
            }
#ifdef SSL_OP_NO_TICKET
            if (ctmp._enable_tickets) {
                SSL_CTX_clear_options(_ssl_ctx, SSL_OP_NO_TICKET);
            } else {
                SSL_CTX_set_options(_ssl_ctx, SSL_OP_NO_TICKET);
            }
#endif
        }
        _ssl = SSL_new(_ssl_ctx);
        SSL_set_tlsext_host_name(_ssl, _host.c_str());
        if (!_host.empty()) {
            X509_VERIFY_PARAM* param = SSL_get0_param(_ssl);
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
    SSL_CTX* _ssl_ctx;
    SSL* _ssl;
#endif
};
