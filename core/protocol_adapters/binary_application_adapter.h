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
// ============================================================================

// Simple TLV framing: [4-byte protocol id][4-byte payload length][payload].
class BinaryApplicationAdapter : public ::base_data_process {
public:
    // TLV header size and maximum accepted payload length (16 MiB).
    static const size_t kBinaryHeaderLen = 8u;
    static const size_t kMaxBinaryFrameLen = 16u * 1024u * 1024u;

    BinaryApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

    virtual ~BinaryApplicationAdapter();

    size_t process_recv_buf(const char* buf, size_t len) override;
    std::string* get_send_buf() override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void peer_close() override;
    void reset() override;
    void destroy() override;

    static bool can_handle(const char* buf, size_t len);

    static std::unique_ptr<::base_data_process> create(
        std::shared_ptr<base_net_obj> conn);

    void set_handler(IApplicationHandler* handler) { _handler = handler; }

private:
    size_t parse_tlv_message();

    void send_binary_response(uint32_t proto_id, const std::vector<uint8_t>& data);

    IApplicationHandler* _handler;
    std::string _recv_buffer;
    std::deque<std::string> _send_queue;
    ConnectionInfo _conn_info;
};

} // namespace myframe

#endif // __BINARY_APPLICATION_ADAPTER_H__
