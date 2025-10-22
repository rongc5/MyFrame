// MyFrame Unified Protocol Architecture - Binary Level 1 Adapter
// Adapts base_data_process to IApplicationHandler interface
#ifndef __BINARY_APPLICATION_ADAPTER_H__
#define __BINARY_APPLICATION_ADAPTER_H__

#include "../app_handler_v2.h"
#include "../base_data_process.h"
#include <memory>
#include <deque>
#include <cstring>

namespace myframe {

// ============================================================================
// Binary Application Adapter (Level 1)
// �� base_data_process ���䵽 IApplicationHandler �ӿ�
// ֧�ּ򵥵� TLV��Tag-Length-Value����ʽ
// ============================================================================

class BinaryApplicationAdapter : public ::base_data_process {
public:
    BinaryApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

    virtual ~BinaryApplicationAdapter();

    // base_data_process �ӿ�ʵ��
    size_t process_recv_buf(const char* buf, size_t len) override;
    std::string* get_send_buf() override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void peer_close() override;
    void reset() override;
    void destroy() override;

    // Э���ⷽ������ UnifiedProtocolFactory ʹ�ã�
    static bool can_handle(const char* buf, size_t len);

    // ������������ UnifiedProtocolFactory ʹ�ã�
    static std::unique_ptr<::base_data_process> create(
        std::shared_ptr<base_net_obj> conn);

    // ���� Handler
    void set_handler(IApplicationHandler* handler) { _handler = handler; }

private:
    // TLV ��ʽ��[4�ֽ�Э��ID][4�ֽڳ���][����...]
    // Э����ͽ���
    size_t parse_tlv_message();

    // ��Ӧ����
    void send_binary_response(uint32_t proto_id, const std::vector<uint8_t>& data);

    // ��Ա����
    IApplicationHandler* _handler;
    std::string _recv_buffer;
    std::deque<std::string> _send_queue;
    std::string _current_send;
    ConnectionInfo _conn_info;
};

} // namespace myframe

#endif // __BINARY_APPLICATION_ADAPTER_H__
