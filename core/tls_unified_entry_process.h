// MyFrame Unified Protocol Architecture - TLS Entry Process
// Handles TLS handshake and then delegates to ProtocolDetector
#pragma once

#include "base_data_process.h"
#include "unified_protocol_factory.h"
#include "ssl_context.h"
#include <vector>
#include <memory>

namespace myframe {

class TlsUnifiedEntryProcess : public ::base_data_process {
public:
    TlsUnifiedEntryProcess(
        std::shared_ptr<base_net_obj> conn,
        const std::vector<UnifiedProtocolFactory::ProtocolEntry>& protocols)
        : base_data_process(conn)
        , _protocols(protocols)
        , _installed(false) {}

    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override { return nullptr; }

private:
    bool ensure_ssl_installed();
    bool init_server_ssl(SSL*& out_ssl);

    std::vector<UnifiedProtocolFactory::ProtocolEntry> _protocols;
    bool _installed;
};

} // namespace myframe
