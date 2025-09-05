#pragma once

#include "base_data_process.h"
#include "app_handler.h"
#include <vector>

class IProtocolProbe;

class protocol_detect_process : public base_data_process
{
public:
    protocol_detect_process(std::shared_ptr<base_net_obj> base_obj, IAppHandler* app_handler = 0, bool over_tls = false);
    virtual ~protocol_detect_process();
    
    void set_app_handler(IAppHandler* handler) { _app_handler = handler; }
    void add_probe(std::unique_ptr<IProtocolProbe> probe) { _probes.push_back(std::move(probe)); }
    void set_detect_timeout(uint64_t ms) { _detect_timeout_ms = ms; }

    virtual size_t process_recv_buf(const char* buf, size_t buf_len) override;
    virtual std::string* get_send_buf() override { return 0; }
    virtual void handle_timeout(std::shared_ptr<timer_msg>& t) override;
    // legacy base has no peek preference hook

private:
    bool _protocol_detected;
    bool _over_tls;
    IAppHandler* _app_handler;
    std::vector<std::unique_ptr<IProtocolProbe>> _probes;
    uint64_t _detect_timeout_ms;
    uint64_t _start_ms;
    uint32_t _timer_id;
};
