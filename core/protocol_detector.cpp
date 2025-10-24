// MyFrame Unified Protocol Architecture - Protocol Detector Implementation
#include "protocol_detector.h"
#include "common_def.h"
#include <algorithm>

namespace myframe {

ProtocolDetector::ProtocolDetector(
    std::shared_ptr<base_net_obj> conn,
    const std::vector<UnifiedProtocolFactory::ProtocolEntry>& protocols,
    bool over_tls)
    : ::base_data_process(conn)
    , _protocols(protocols)
    , _over_tls(over_tls)
    , _detected(false)
{
    // 协议已在传入前排序，不需要再次排序
    // std::sort() 可能导致 std::function 对象的移动/拷贝问题
    PDEBUG("[ProtocolDetector] Created with %zu protocols", _protocols.size());
}

ProtocolDetector::~ProtocolDetector() {
    PDEBUG("[ProtocolDetector] Destroyed, detected=%d, protocol=%s",
           _detected, _detected_protocol.c_str());
}

size_t ProtocolDetector::process_recv_buf(const char* buf, size_t len) {
    if (!_detected) {
        // 还未检测到协议，缓存数据
        _buffer.append(buf, len);

        PDEBUG("[ProtocolDetector] Buffering data, size=%zu", _buffer.size());

        // 检查缓冲区大小，防止恶意攻击
        if (_buffer.size() > MAX_DETECT_BUFFER_SIZE) {
            PDEBUG("[ProtocolDetector] Buffer overflow (size=%zu), closing connection",
                   _buffer.size());
            peer_close();
            return 0;
        }

        // 遍历所有协议进行检测
        for (const auto& proto : _protocols) {
            if (proto.detect(_buffer.data(), _buffer.size())) {
                // 协议匹配成功
                PDEBUG("[ProtocolDetector] Protocol '%s' detected", proto.name.c_str());

                _detected_protocol = proto.name;
                _detected = true;

                // 创建实际的协议处理器
                _delegate = proto.create(get_base_net());

                if (!_delegate) {
                    PDEBUG("[ProtocolDetector] Failed to create protocol handler for '%s'",
                           proto.name.c_str());
                    peer_close();
                    return 0;
                }

                // 将缓冲的数据交给协议处理器，可能只消费部分数据
                size_t total_consumed = 0;

                // CRITICAL: 缓存 buffer 相关数据到局部变量
                // 因为 _delegate->process_recv_buf() 可能会调用 set_process() 删除 this 对象
                // 之后不能再访问任何成员变量（_buffer, _delegate 等）
                const char* buffer_data = _buffer.data();
                size_t buffer_size = _buffer.size();

                // 循环处理直到处理器无法继续消费
                while (total_consumed < buffer_size) {
                    size_t available = buffer_size - total_consumed;
                    size_t consumed = _delegate->process_recv_buf(
                        buffer_data + total_consumed, available);

                    if (consumed == 0) {
                        // 处理器暂时无法消费更多数据，保留剩余部分
                        break;
                    }

                    if (consumed > available) {
                        // 防御性检查：不允许超量消费
                        consumed = available;
                    }

                    total_consumed += consumed;

                    // 如果还有剩余数据，将在下一次循环尝试继续处理
                }

                // 注意：如果 delegate 调用了 set_process()，this 对象可能已被删除
                // 此时不能访问任何成员变量！
                // 当 total_consumed == buffer_size 时，说明所有数据都被消费，
                // delegate 可能已经替换了 this，所以直接返回，不再访问 _buffer
                if (total_consumed >= buffer_size) {
                    // 所有数据都被消费，直接返回（可能 this 已被删除）
                    return len;
                }

                // 如果没有消费所有数据，说明 delegate 没有替换 this，可以安全访问成员
                if (total_consumed > 0) {
                    _buffer.erase(0, total_consumed);
                    PDEBUG("[ProtocolDetector] Consumed %zu bytes, %zu bytes remaining",
                           total_consumed, _buffer.size());
                }

                // 返回本次实际接收的长度
                return len;
            }
        }

        // 还没有匹配的协议，等待更多数据
        return len;
    }

    // 已经检测到协议，直接转发给协议处理器
    if (_delegate) {
        return _delegate->process_recv_buf(buf, len);
    }

    return len;
}

std::string* ProtocolDetector::get_send_buf() {
    if (_delegate) {
        return _delegate->get_send_buf();
    }
    return nullptr;
}

void ProtocolDetector::handle_msg(std::shared_ptr<::normal_msg>& msg) {
    if (_delegate) {
        _delegate->handle_msg(msg);
    }
}

void ProtocolDetector::handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
    if (_delegate) {
        _delegate->handle_timeout(t_msg);
    } else {
        // 检测超时，关闭连接
        PDEBUG("[ProtocolDetector] Detection timeout, closing connection");
        peer_close();
    }
}

void ProtocolDetector::peer_close() {
    if (_delegate) {
        _delegate->peer_close();
    }
    base_data_process::peer_close();
}

void ProtocolDetector::reset() {
    _detected = false;
    _buffer.clear();
    _detected_protocol.clear();

    if (_delegate) {
        _delegate->reset();
    }

    base_data_process::reset();
}

void ProtocolDetector::destroy() {
    if (_delegate) {
        _delegate->destroy();
    }
    base_data_process::destroy();
}

} // namespace myframe
