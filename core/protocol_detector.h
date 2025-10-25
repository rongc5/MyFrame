// MyFrame Unified Protocol Architecture - Protocol Detector
// Automatic protocol detection and delegation
#ifndef __PROTOCOL_DETECTOR_H__
#define __PROTOCOL_DETECTOR_H__

#include "base_data_process.h"
#include "base_connect.h"
#include "unified_protocol_factory.h"
#include <string>
#include <vector>
#include <memory>

namespace myframe {

// ============================================================================
// Protocol Detector
// 协议检测器 - 自动检测协议并创建相应的处理器
// ============================================================================

class ProtocolDetector : public ::base_data_process {
public:
    ProtocolDetector(
        std::shared_ptr<base_net_obj> conn,
        const std::vector<UnifiedProtocolFactory::ProtocolEntry>& protocols,
        bool over_tls = false);

    virtual ~ProtocolDetector();

    size_t process_recv_buf(const char* buf, size_t len) override;
    std::string* get_send_buf() override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void peer_close() override;
    void reset() override;
    void destroy() override;

    bool want_peek() const override { return !_over_tls && !_detected; }

private:
    bool handoff_to_protocol(const UnifiedProtocolFactory::ProtocolEntry& proto,
                             base_connect<base_data_process>* holder,
                             const char* data,
                             size_t len);

    std::vector<UnifiedProtocolFactory::ProtocolEntry> _protocols;
    bool _over_tls;
    bool _detected;
    std::string _buffer;
    uint64_t _start_ms;
    uint32_t _timer_id;

    static const size_t MAX_DETECT_BUFFER_SIZE = 4096;
    static const uint64_t DETECT_TIMEOUT_MS = 5000;
};

} // namespace myframe

#endif // __PROTOCOL_DETECTOR_H__
