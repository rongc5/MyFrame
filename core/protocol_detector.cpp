// MyFrame Unified Protocol Architecture - Protocol Detector Implementation
#include "protocol_detector.h"
#include "base_net_obj.h"
#include "common_def.h"
#include "common_exception.h"
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
    , _start_ms(0)
    , _timer_id(0)
{
    PDEBUG("[ProtocolDetector] Created with %zu protocols (over_tls=%d)",
           _protocols.size(), _over_tls ? 1 : 0);
}

ProtocolDetector::~ProtocolDetector() {
    PDEBUG("[ProtocolDetector] Destroyed (detected=%d)", _detected);
}

bool ProtocolDetector::handoff_to_protocol(const UnifiedProtocolFactory::ProtocolEntry& proto,
                                           base_connect<base_data_process>* holder,
                                           const char* data,
                                           size_t len)
{
    std::unique_ptr<::base_data_process> next = proto.create(get_base_net());
    if (!next) {
        PDEBUG("[ProtocolDetector] Failed to create handler for '%s'", proto.name.c_str());
        return false;
    }

    auto net = get_base_net();
    if (net) {
        net->set_protocol_tag(proto.name, proto.terminal);
    }

    auto* raw = next.release();
    // CRITICAL: set_process() deletes the previous process (i.e. this);
    // no member variable may be accessed afterwards!
    holder->set_process(raw);
    // 'this' has been freed: only locals and the raw pointer may be used
    if (len > 0) {
        size_t consumed = raw->process_recv_buf(data, len);
        (void)consumed;
    }
    // Do not write _detected = true; (use-after-free!)
    return true;
}

size_t ProtocolDetector::process_recv_buf(const char* buf, size_t len) {
    if (_detected) {
        return len;
    }

    if (_start_ms == 0) {
        _start_ms = GetMilliSecond();
        std::shared_ptr<timer_msg> t(new timer_msg);
        t->_obj_id = get_base_net()->get_id()._id;
        // This timer can outlive this detector after set_process() hands the
        // connection to HTTP/WebSocket.  Keep it distinct from the connection
        // idle timeout so the new protocol can safely ignore the stale event.
        t->_timer_type = PROTOCOL_DETECT_TIMER_TYPE;
        t->_time_length = DETECT_TIMEOUT_MS;
        add_timer(t);
        _timer_id = t->_timer_id;
    }

    if (len == 0) {
        return 0;
    }

    base_connect<base_data_process>* holder =
        dynamic_cast<base_connect<base_data_process>*>(get_base_net().get());
    if (!holder) {
        PDEBUG("[ProtocolDetector] holder cast failed");
        notify_peer_close();
        return len;
    }

    // Try detection on incoming data directly before buffering.
    // This avoids rejecting large payloads (e.g. HTTP POST with body > 4KB)
    // that can be identified from the first few bytes.
    if (_buffer.empty()) {
        for (const auto& proto : _protocols) {
            if (proto.detect(buf, len)) {
                bool ok = handoff_to_protocol(proto, holder, buf, len);
                if (!ok) {
                    notify_peer_close();
                }
                // After a successful handoff 'this' is deleted: do not touch members
                return len;
            }
        }
    }

    // Buffer the data for multi-packet detection
    size_t remaining = (MAX_DETECT_BUFFER_SIZE > _buffer.size())
                           ? (MAX_DETECT_BUFFER_SIZE - _buffer.size())
                           : 0;
    if (remaining == 0 || len > remaining) {
        PDEBUG("[ProtocolDetector] Buffer overflow attempt (current=%zu, incoming=%zu, cap=%zu). Closing.",
               _buffer.size(), static_cast<size_t>(len), static_cast<size_t>(MAX_DETECT_BUFFER_SIZE));
        notify_peer_close();
        // notify_peer_close() only flags the process; throw so the container
        // tears the connection down instead of sniffing unbounded input.
        THROW_COMMON_EXCEPT("protocol detect buffer overflow (" << (_buffer.size() + len) << ")");
    }

    _buffer.append(buf, len);

    for (const auto& proto : _protocols) {
        if (proto.detect(_buffer.data(), _buffer.size())) {
            std::string buffered;
            buffered.swap(_buffer);
            bool ok = handoff_to_protocol(proto, holder,
                                          buffered.data(), buffered.size());
            if (!ok) {
                notify_peer_close();
            }
            // After a successful handoff 'this' is deleted: do not touch members
            return len;
        }
    }

    return len;
}

std::string* ProtocolDetector::get_send_buf() { return nullptr; }
void ProtocolDetector::handle_msg(std::shared_ptr<::normal_msg>&) {}

void ProtocolDetector::handle_timeout(std::shared_ptr<::timer_msg>& t_msg) {
    if (t_msg && t_msg->_timer_id == _timer_id && !_detected) {
        PDEBUG("[ProtocolDetector] Detection timeout");
        notify_peer_close();
        THROW_COMMON_EXCEPT("protocol detect timeout");
    }
}

void ProtocolDetector::peer_close() {
    base_data_process::peer_close();
}

void ProtocolDetector::reset() {
    _detected = false;
    _buffer.clear();
    _start_ms = 0;
    _timer_id = 0;
    if (auto net = get_base_net()) {
        if (!net->protocol_locked()) {
            net->clear_protocol_tag();
        }
    }
    base_data_process::reset();
}

void ProtocolDetector::destroy() {
    base_data_process::destroy();
}

} // namespace myframe
