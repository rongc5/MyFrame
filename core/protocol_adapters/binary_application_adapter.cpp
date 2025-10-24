// MyFrame Unified Protocol Architecture - Binary Level 1 Adapter Implementation
#include "binary_application_adapter.h"
#include "../base_net_obj.h"
#include <cstring>
#include <algorithm>

namespace myframe {

// ============================================================================
// BinaryApplicationAdapter Implementation
// ============================================================================

BinaryApplicationAdapter::BinaryApplicationAdapter(
    std::shared_ptr<base_net_obj> conn,
    IApplicationHandler* handler)
    : ::base_data_process(conn), _handler(handler) {
    // ��ʼ��������Ϣ
    if (conn) {
        // ע��base_net_obj ��ǰ���ṩ IP/�˿ڷ��ʽӿ�
        _conn_info.remote_ip = "";
        _conn_info.remote_port = 0;
        _conn_info.local_ip = "";
        _conn_info.local_port = 0;
        _conn_info.connection_id = conn->get_id()._id;
    }

    // �����������ڹ���
    if (_handler) {
        _handler->on_connect(_conn_info);
    }
}

BinaryApplicationAdapter::~BinaryApplicationAdapter() {
    // �������Ͷ���
    _send_queue.clear();
}

bool BinaryApplicationAdapter::can_handle(const char* buf, size_t len) {
    // �򵥵Ķ�����Э����
    // ����Ƿ�Ϊ TLV ��ʽ��������Ҫ 8 �ֽ�ͷ����4�ֽ�ID + 4�ֽڳ��ȣ�
    if (len < 8) return false;

    // ��ȡ�����ֶ�
    uint32_t length = 0;
    std::memcpy(&length, buf + 4, sizeof(uint32_t));

    // ��鳤�ȵĺ����ԣ����� > 0 �� < 1MB��
    return length > 0 && length < (1 << 20);
}

std::unique_ptr<::base_data_process> BinaryApplicationAdapter::create(
    std::shared_ptr<base_net_obj> conn) {
    return std::unique_ptr<::base_data_process>(new BinaryApplicationAdapter(conn, nullptr));
}

size_t BinaryApplicationAdapter::process_recv_buf(const char* buf, size_t len) {
    if (!_handler) return len;

    _recv_buffer.append(buf, len);
    size_t consumed = 0;

    // ����������������Ϣ
    while (_recv_buffer.size() >= 8) {
        size_t result = parse_tlv_message();
        if (result == 0) break;  // ���������ȴ���������
        consumed += result;
    }

    return consumed > 0 ? consumed : len;
}

size_t BinaryApplicationAdapter::parse_tlv_message() {
    if (_recv_buffer.size() < 8) return 0;

    // ���� TLV ͷ��
    uint32_t proto_id = 0;
    uint32_t data_len = 0;
    std::memcpy(&proto_id, _recv_buffer.data(), sizeof(uint32_t));
    std::memcpy(&data_len, _recv_buffer.data() + 4, sizeof(uint32_t));

    // ��������Ƿ�����
    if (_recv_buffer.size() < 8 + data_len) return 0;

    // �����������
    BinaryRequest req;
    req.protocol_id = proto_id;
    req.payload.assign(
        reinterpret_cast<const uint8_t*>(_recv_buffer.data() + 8),
        reinterpret_cast<const uint8_t*>(_recv_buffer.data() + 8 + data_len));

    // ������Ӧ����
    BinaryResponse res;

    try {
        // �����û�������
        detail::HandlerContextScope scope(this);
        _handler->on_binary(req, res);

        // ������Ӧ
        if (!res.data.empty()) {
            send_binary_response(proto_id, res.data);
        }
    } catch (const std::exception& e) {
        _handler->on_error(std::string("Binary handler exception: ") + e.what());
    }

    // �ӻ�����ɾ���Ѵ�������Ϣ
    _recv_buffer.erase(0, 8 + data_len);
    return 8 + data_len;
}

void BinaryApplicationAdapter::send_binary_response(
    uint32_t proto_id,
    const std::vector<uint8_t>& data) {
    std::string frame;
    frame.reserve(8 + data.size());

    // д��Э�� ID �ͳ���
    uint32_t length = data.size();
    frame.append(reinterpret_cast<const char*>(&proto_id), sizeof(uint32_t));
    frame.append(reinterpret_cast<const char*>(&length), sizeof(uint32_t));

    // д������
    if (!data.empty()) {
        frame.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    // ���ӵ����Ͷ���
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
        _handler->handle_msg(msg);
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
