// MyFrame Unified Protocol Architecture - Protocol Detector
// Automatic protocol detection and delegation
#ifndef __PROTOCOL_DETECTOR_H__
#define __PROTOCOL_DETECTOR_H__

#include "base_data_process.h"
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
    // 构造函数
    ProtocolDetector(
        std::shared_ptr<base_net_obj> conn,
        const std::vector<UnifiedProtocolFactory::ProtocolEntry>& protocols);

    virtual ~ProtocolDetector();

    // base_data_process 接口实现
    virtual size_t process_recv_buf(const char* buf, size_t len) override;
    virtual std::string* get_send_buf() override;
    virtual void handle_msg(std::shared_ptr<::normal_msg>& msg) override;
    virtual void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    virtual void peer_close() override;
    virtual void reset() override;
    virtual void destroy() override;

    // 查询接口
    bool is_detected() const { return _detected; }
    const std::string& detected_protocol() const { return _detected_protocol; }

private:
    std::vector<UnifiedProtocolFactory::ProtocolEntry> _protocols;     // 协议列表（已排序）
    std::unique_ptr<::base_data_process> _delegate; // 实际的协议处理器
    bool _detected;                             // 是否已检测到协议
    std::string _buffer;                        // 检测缓冲区
    std::string _detected_protocol;             // 检测到的协议名称

    // 安全限制
    static const size_t MAX_DETECT_BUFFER_SIZE = 4096;  // 最大检测缓冲区 4KB
    static const uint64_t DETECT_TIMEOUT_MS = 5000;     // 检测超时 5秒
};

} // namespace myframe

#endif // __PROTOCOL_DETECTOR_H__
