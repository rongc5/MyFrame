// MyFrame Unified Protocol Architecture - Binary Level 1 Adapter Implementation
#include "binary_application_adapter.h"
#include "../common_exception.h"
#include "../base_net_obj.h"
#include <cstring>
#include <algorithm>

namespace myframe {

const size_t BinaryApplicationAdapter::kBinaryHeaderLen;
const size_t BinaryApplicationAdapter::kMaxBinaryFrameLen;

// ============================================================================
// BinaryApplicationAdapter Implementation
// ============================================================================

BinaryApplicationAdapter::BinaryApplicationAdapter(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
    : ::base_data_process(conn), _handler(handler) {
    if (conn) {
        _conn_info.remote_ip = "";
        _conn_info.remote_port = 0;
        _conn_info.local_ip = "";
        _conn_info.local_port = 0;
        _conn_info.connection_id = conn->get_id()._id;
    }

    if (_handler) {
        _handler->on_connect(_conn_info);
    }
}

BinaryApplicationAdapter::~BinaryApplicationAdapter() {
    _send_queue.clear();
}

bool BinaryApplicationAdapter::can_handle(const char* buf, size_t len) {
    if (len < 8) return false;

    uint32_t length = 0;
    std::memcpy(&length, buf + 4, sizeof(uint32_t));

    return length > 0 && length < (1 << 20);
}

std::unique_ptr<::base_data_process> BinaryApplicationAdapter::create(
    std::shared_ptr<base_net_obj> conn) {
    return std::unique_ptr<::base_data_process>(new BinaryApplicationAdapter(conn, nullptr));
}

size_t BinaryApplicationAdapter::process_recv_buf(const char* buf, size_t len) {
    if (!_handler) return len;

    _recv_buffer.append(buf, len);

    // Parse every complete message currently buffered.
    while (_recv_buffer.size() >= kBinaryHeaderLen) {
        size_t result = parse_tlv_message();
        if (result == 0) break;  // incomplete message: wait for more data
    }

    // All len bytes now live in _recv_buffer, so they are always consumed.
    // Returning less would make base_connect feed the tail a second time.
    return len;
}

size_t BinaryApplicationAdapter::parse_tlv_message() {
    if (_recv_buffer.size() < kBinaryHeaderLen) return 0;

    // TLV header: [4-byte protocol id][4-byte payload length] (host order).
    uint32_t proto_id = 0;
    uint32_t data_len = 0;
    std::memcpy(&proto_id, _recv_buffer.data(), sizeof(uint32_t));
    std::memcpy(&data_len, _recv_buffer.data() + 4, sizeof(uint32_t));

    // Reject oversized frames before any size arithmetic or buffering: the
    // frame length is computed in size_t, never as "8 + data_len" in uint32_t.
    if (static_cast<size_t>(data_len) > kMaxBinaryFrameLen) {
        THROW_COMMON_EXCEPT("binary frame too large (" << data_len << " > " << kMaxBinaryFrameLen << ")");
    }
    const size_t frame_len = kBinaryHeaderLen + static_cast<size_t>(data_len);

    // Is the whole message available yet?
    if (_recv_buffer.size() < frame_len) return 0;

    BinaryRequest req;
    req.protocol_id = proto_id;
    req.payload.assign(
        reinterpret_cast<const uint8_t*>(_recv_buffer.data() + kBinaryHeaderLen),
        reinterpret_cast<const uint8_t*>(_recv_buffer.data() + frame_len));

    BinaryResponse res;

    try {
        detail::HandlerContextScope scope(this);
        _handler->on_binary(req, res);

        if (!res.data.empty()) {
            send_binary_response(proto_id, res.data);
        }
    } catch (const std::exception& e) {
        _handler->on_error(std::string("Binary handler exception: ") + e.what());
    }

    // Drop the processed message from the buffer.
    _recv_buffer.erase(0, frame_len);
    return frame_len;
}

void BinaryApplicationAdapter::send_binary_response(
    uint32_t proto_id,
    const std::vector<uint8_t>& data) {
    std::string frame;
    frame.reserve(8 + data.size());

    uint32_t length = data.size();
    frame.append(reinterpret_cast<const char*>(&proto_id), sizeof(uint32_t));
    frame.append(reinterpret_cast<const char*>(&length), sizeof(uint32_t));

    if (!data.empty()) {
        frame.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    _send_queue.push_back(frame);
}

std::string* BinaryApplicationAdapter::get_send_buf() {
    if (_send_queue.empty()) {
        return nullptr;
    }

    std::string* frame = new std::string(std::move(_send_queue.front()));
    _send_queue.pop_front();
    return frame;
}

void BinaryApplicationAdapter::handle_msg(std::shared_ptr<normal_msg>& msg) {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_thread_msg(msg);
    }
}

void BinaryApplicationAdapter::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->handle_timeout(t_msg);
    }
}

void BinaryApplicationAdapter::peer_close() {
    if (_handler) {
        detail::HandlerContextScope scope(this);
        _handler->on_disconnect();
    }
    base_data_process::peer_close();
}

void BinaryApplicationAdapter::reset() {
    _recv_buffer.clear();
    _send_queue.clear();
    base_data_process::reset();
}

void BinaryApplicationAdapter::destroy() {
    base_data_process::destroy();
}

} // namespace myframe
