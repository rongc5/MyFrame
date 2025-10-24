// MyFrame Unified Protocol Architecture - TLS Entry Process Implementation
#include "tls_unified_entry_process.h"
#include "protocol_detector.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "codec.h"
#include <sys/stat.h>
#include <cstdlib>
#include <utility>

namespace myframe {

static bool file_exists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool TlsUnifiedEntryProcess::init_server_ssl(SSL*& out_ssl) {
#ifdef ENABLE_SSL
    ssl_context* ctx = ssl_context_singleton::get_instance_ex();
    if (!ctx->is_initialized()) {
        ssl_config conf;
        if (!tls_get_server_config(conf)) {
            // 环境变量兜底
            const char* env_cert = ::getenv("MYFRAME_SSL_CERT");
            const char* env_key  = ::getenv("MYFRAME_SSL_KEY");
            const char* env_protos = ::getenv("MYFRAME_SSL_PROTOCOLS");
            const char* env_cipher = ::getenv("MYFRAME_SSL_CIPHERS");
            const char* env_verify = ::getenv("MYFRAME_SSL_VERIFY");

            if (env_cert && env_key && file_exists(env_cert) && file_exists(env_key)) {
                conf._cert_file = env_cert;
                conf._key_file  = env_key;
                conf._protocols = env_protos ? env_protos : "TLSv1.2,TLSv1.3";
                if (env_cipher) conf._cipher_list = env_cipher;
                if (env_verify) {
                    if (strcmp(env_verify, "1") == 0 || strcasecmp(env_verify, "true") == 0)
                        conf._verify_peer = true;
                    else
                        conf._verify_peer = false;
                } else {
                    conf._verify_peer = false;
                }
            } else {
                // 常见默认路径兜底
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
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PDEBUG("[TlsUnified] No TLS certificates found");
                    return false;
                }
            }
        }
        if (!ctx->init_server(conf)) {
            PDEBUG("[TlsUnified] init_server failed (cert/key load or options)");
            return false;
        }
    }

    SSL* ssl = ctx->create_ssl();
    if (!ssl) {
        PDEBUG("[TlsUnified] Failed to create SSL object");
        return false;
    }

    int fd = get_base_net()->get_sfd();
    if (SSL_set_fd(ssl, fd) != 1) {
        PDEBUG("[TlsUnified] SSL_set_fd failed");
        SSL_free(ssl);
        return false;
    }

    PDEBUG("[TlsUnified] Binding SSL to fd=%d", fd);
    SSL_set_accept_state(ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    out_ssl = ssl;
    return true;
#else
    (void)out_ssl;
    PDEBUG("[TlsUnified] SSL support not compiled in");
    return false;
#endif
}

bool TlsUnifiedEntryProcess::ensure_ssl_installed() {
#ifdef ENABLE_SSL
    if (_installed) return true;

    SSL* ssl = nullptr;
    if (!init_server_ssl(ssl)) {
        PDEBUG("[TlsUnified] Failed to initialize server SSL");
        return false;
    }

    // 获取连接对象
    base_connect<base_data_process>* holder =
        dynamic_cast<base_connect<base_data_process>*>(get_base_net().get());
    if (!holder) {
        PDEBUG("[TlsUnified] Failed to cast to base_connect");
        SSL_free(ssl);
        return false;
    }

    // 在连接上安装 SSL 编解码器
    PDEBUG("[TlsUnified] Installing SslCodec and switching to over-TLS detector");
    holder->set_codec(std::unique_ptr<ICodec>(new SslCodec(ssl)));

    // 构造后尝试一次握手
    {
        SslCodec* sc = static_cast<SslCodec*>(holder->get_codec());
        if (sc) (void)sc->ssl_handshake();
    }

    // 确保尽快触发 SSL_accept 的可写阶段
    holder->update_event(holder->get_event() | EPOLLOUT);

    // 切到协议检测（TLS 之上继续探测 HTTP/WS/Binary等）
    // 注意：不包括 TLS 本身，避免无限递归
    std::vector<UnifiedProtocolFactory::ProtocolEntry> inner_protocols;
    for (const auto& proto : _protocols) {
        if (proto.name != "TLS") {  // 排除 TLS 自身
            inner_protocols.push_back(proto);
        }
    }

    std::unique_ptr<ProtocolDetector> detector(
        new ProtocolDetector(get_base_net(), inner_protocols, true));

    // Mark as installed before handing ownership away; this object will be
    // destroyed once set_process() replaces it.
    _installed = true;
    PDEBUG("[TlsUnified] Successfully switched to ProtocolDetector with %zu protocols",
           inner_protocols.size());
    holder->set_process(std::move(detector));
    return true;
#else
    return false;
#endif
}

size_t TlsUnifiedEntryProcess::process_recv_buf(const char* buf, size_t len) {
    // 安装SSL并切换到TLS之上的协议探测
    // 消费所有数据，因为SSL会重新读取
    (void)buf;
    (void)len;

    // CRITICAL: 检查是否已安装，避免重复调用导致 use-after-free
    // 一旦 ensure_ssl_installed() 成功，它会调用 set_process() 删除 this 对象
    // 因此我们必须在调用前检查，并且调用后不能访问任何成员变量
    if (_installed) {
        // 已经安装过，不应该再被调用，但为安全起见返回
        return len;
    }

    if (!ensure_ssl_installed()) {
        PDEBUG("[TlsUnified] Failed to install SSL, consuming %zu bytes", len);
        return len;
    }

    // 注意：执行到这里时，this 已经被 set_process() 删除！
    // 不能访问任何成员变量，只能返回局部变量
    return len; // 消费用户缓冲，避免后续 over‑TLS 探测被明文阻塞
}

} // namespace myframe
