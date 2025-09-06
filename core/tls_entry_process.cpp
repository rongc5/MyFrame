#include "tls_entry_process.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "protocol_detect_process.h"
#include "protocol_probes.h"
#include "codec.h"
#include <sys/stat.h>
#include <cstdlib>

static bool file_exists(const char* path) { struct stat st; return ::stat(path, &st) == 0 && S_ISREG(st.st_mode); }

bool tls_entry_process::init_server_ssl(SSL*& out_ssl) {
#ifdef ENABLE_SSL
    ssl_context* ctx = ssl_context_singleton::get_instance_ex();
    if (!ctx->is_initialized()) {
        ssl_config conf; 
        if (!tls_get_server_config(conf)) {
            // 环境变量兜底
            const char* env_cert = ::getenv("MYFRAME_SSL_CERT");
            const char* env_key  = ::getenv("MYFRAME_SSL_KEY");
            const char* env_protos = ::getenv("MYFRAME_SSL_PROTOCOLS"); // 例如: TLSv1.2,TLSv1.3
            const char* env_cipher = ::getenv("MYFRAME_SSL_CIPHERS");   // 例如: HIGH:!aNULL:!MD5
            const char* env_verify = ::getenv("MYFRAME_SSL_VERIFY");    // 1/0 or true/false
            if (env_cert && env_key && file_exists(env_cert) && file_exists(env_key)) {
                conf._cert_file = env_cert; 
                conf._key_file  = env_key; 
                conf._protocols = env_protos ? env_protos : "TLSv1.2,TLSv1.3"; 
                if (env_cipher) conf._cipher_list = env_cipher;
                if (env_verify) {
                    if (strcmp(env_verify, "1") == 0 || strcasecmp(env_verify, "true") == 0) conf._verify_peer = true; 
                    else conf._verify_peer = false;
                } else {
                    conf._verify_peer = false;
                }
            } else {
                // 常见默认路径兜底（项目根、test、tests/common/cert）
                const char* candidates[][2] = {
                    {"server.crt", "server.key"},
                    {"./test/server.crt", "./test/server.key"},
                    {"test_certs/server.crt", "test_certs/server.key"},
                    {"tests/common/cert/server.crt", "tests/common/cert/server.key"},
                };
                bool found = false;
                for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
                    if (file_exists(candidates[i][0]) && file_exists(candidates[i][1])) {
                        conf._cert_file = candidates[i][0];
                        conf._key_file  = candidates[i][1];
                        conf._protocols = "TLSv1.2,TLSv1.3";
                        found = true; break;
                    }
                }
                if (!found) return false;
            }
        }
        if (!ctx->init_server(conf)) { PDEBUG("[tls] init_server failed (cert/key load or options)"); return false; }
    }
    SSL* ssl = ctx->create_ssl(); if (!ssl) return false;
    int fd = get_base_net()->get_sfd(); if (SSL_set_fd(ssl, fd) != 1) { PDEBUG("[tls] SSL_set_fd failed"); SSL_free(ssl); return false; }
    PDEBUG("[tls] Binding SSL to fd=%d", fd);
    SSL_set_accept_state(ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    out_ssl = ssl; return true;
#else
    (void)out_ssl; return false;
#endif
}

bool tls_entry_process::ensure_ssl_installed() {
#ifdef ENABLE_SSL
    if (_installed) return true;
    SSL* ssl = 0; if (!init_server_ssl(ssl)) return false;
    // 安装SSL后切回探测
    base_connect<base_data_process>* holder = dynamic_cast< base_connect<base_data_process>* >(get_base_net().get());
    if (!holder) return false;
    // 在连接上安装 SSL 编解码器
    PDEBUG("[tls] Installing SslCodec and switching to over-TLS detector");
    holder->set_codec(std::unique_ptr<ICodec>(new SslCodec(ssl)));
    // 构造后尝试一次握手，记录握手状态（忽略 WANT_READ/WRITE）
    {
        SslCodec* sc = static_cast<SslCodec*>(holder->get_codec());
        if (sc) (void)sc->ssl_handshake();
    }
    // 确保尽快触发 SSL_accept 的可写阶段
    holder->update_event(holder->get_event() | EPOLLOUT);

    // 切到探测（TLS 之上继续探测 HTTP/WS/自定义）
    std::unique_ptr<protocol_detect_process> detector(new protocol_detect_process(get_base_net(), _app_handler, true));
    detector->add_probe(std::unique_ptr<IProtocolProbe>(new WsProbe(_app_handler)));
    // Prefer HTTP/2 when client sends h2 preface
    detector->add_probe(std::unique_ptr<IProtocolProbe>(new Http2Probe(_app_handler)));
    detector->add_probe(std::unique_ptr<IProtocolProbe>(new HttpProbe(_app_handler)));
    #ifdef HAS_CUSTOM_PROBE
    detector->add_probe(std::unique_ptr<IProtocolProbe>(new CustomProbe()));
    #endif
    holder->set_process(detector.release());
    _installed = true; return true;
#else
    return false;
#endif
}

size_t tls_entry_process::process_recv_buf(const char* buf, size_t len) {
    // 安装SSL并切换到TLS之上的协议探测；此处应清理当前用户缓冲中已窥视到的明文字节，
    // 因为这些字节仍留在内核缓冲（我们在探测阶段使用了 MSG_PEEK），SSL 将在后续 RECV 中读取它们。
    (void)buf; (void)len;
    if (!ensure_ssl_installed()) return len;
    return len; // 消费用户缓冲，避免后续 over‑TLS 探测被明文阻塞
}
