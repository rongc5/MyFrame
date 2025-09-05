#pragma once

#include <string>
#include <openssl/ssl.h>

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

class ssl_context {
public:
    ssl_context() : _ctx(0), _inited(false) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    }
    ~ssl_context() { if (_ctx) SSL_CTX_free(_ctx); }

    bool is_initialized() const { return _inited; }

    bool init_server(const ssl_config& conf) {
        if (_inited) return true;
        const SSL_METHOD* method = TLS_server_method();
        _ctx = SSL_CTX_new(method);
        if (!_ctx) return false;
        if (SSL_CTX_use_certificate_file(_ctx, conf._cert_file.c_str(), SSL_FILETYPE_PEM) != 1) return false;
        if (SSL_CTX_use_PrivateKey_file(_ctx, conf._key_file.c_str(), SSL_FILETYPE_PEM) != 1) return false;
        if (!conf._cipher_list.empty()) {
            if (SSL_CTX_set_cipher_list(_ctx, conf._cipher_list.c_str()) != 1) return false;
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
