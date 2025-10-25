// MyFrame Unified Protocol Architecture - Protocol Detector Implementation
#include "protocol_detector.h"
#include "common_def.h"
#include <algorithm>

namespace myframe {

namespace {

inline bool handoff_to_protocol(base_connect<base_data_process>* holder,
                                std::unique_ptr<base_data_process> next,
                                const char* data,
                                size_t len)
{
    if (!holder || !next) return false;
    auto* raw = next.release();
    holder->set_process(raw);
    if (len > 0) {
        (void)raw->process_recv_buf(data, len);
    }
    return true;
}

} // namespace

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

    auto* raw = next.release();
    holder->set_process(raw);
    if (len > 0) {
        size_t consumed = raw->process_recv_buf(data, len);
        (void)consumed;
    }
    _detected = true;
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
        t->_timer_type = NONE_DATA_TIMER_TYPE;
        t->_time_length = DETECT_TIMEOUT_MS;
        add_timer(t);
        _timer_id = t->_timer_id;
    }

    _buffer.append(buf, len);
    if (_buffer.size() > MAX_DETECT_BUFFER_SIZE) {
        PDEBUG("[ProtocolDetector] Buffer overflow (> %zu), closing", _buffer.size());
        peer_close();
        return len;
    }

    base_connect<base_data_process>* holder =
        dynamic_cast<base_connect<base_data_process>*>(get_base_net().get());
    if (!holder) {
        PDEBUG("[ProtocolDetector] holder cast failed");
        peer_close();
        return len;
    }

    for (const auto& proto : _protocols) {
        if (proto.detect(_buffer.data(), _buffer.size())) {
            std::string buffered = std::move(_buffer);
            _buffer.clear();
            bool ok = handoff_to_protocol(proto, holder,
                                          buffered.data(), buffered.size());
            if (!ok) {
                peer_close();
                return len;
            }
            _detected = true;
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
        peer_close();
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
    base_data_process::reset();
}

void ProtocolDetector::destroy() {
    base_data_process::destroy();
}

} // namespace myframe
