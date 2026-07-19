#pragma once

#include "base_data_process.h"
#include "app_handler_v2.h"
#include "protocol_probes.h"
#include <vector>

// IProtocolProbe is defined in protocol_probes.h

class protocol_detect_process : public base_data_process
{
public:
    protocol_detect_process(std::shared_ptr<base_net_obj> base_obj, myframe::IApplicationHandler* app_handler = nullptr, bool over_tls = false);
    virtual ~protocol_detect_process();
    
    void set_app_handler(myframe::IApplicationHandler* handler) { _app_handler = handler; }
    void add_probe(std::unique_ptr<IProtocolProbe> probe) { _probes.push_back(std::move(probe)); }
    void set_detect_timeout(uint64_t ms) { _detect_timeout_ms = ms; }

    virtual size_t process_recv_buf(const char* buf, size_t buf_len) override;
    virtual std::string* get_send_buf() override { return 0; }
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t) override;
    // Prefer MSG_PEEK only before protocol is detected and only when not over TLS.
    // When running over TLS (after installing SslCodec), data must be consumed by SSL.
    bool want_peek() const override { return !_protocol_detected && !_over_tls; }

private:
    bool _protocol_detected;
    bool _over_tls;
    myframe::IApplicationHandler* _app_handler;
    std::vector<std::unique_ptr<IProtocolProbe>> _probes;
    // Detection guards
    size_t _total_bytes;
    uint64_t _start_ms;
    uint32_t _timer_id;
    uint64_t _detect_timeout_ms;

    // Reasonable upper-bounds for detection phase
    static constexpr size_t kMaxDetectBytes = 4096;        // 4KB maximum sniff bytes
    static constexpr uint64_t kDetectTimeoutMs = 5000;       // 5s default timeout
};
