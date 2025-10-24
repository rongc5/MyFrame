// MyFrame Unified Protocol Architecture - WebSocket Level 1 Adapter
// Adapts web_socket_res_process to IApplicationHandler interface
#ifndef __WS_APPLICATION_ADAPTER_H__
#define __WS_APPLICATION_ADAPTER_H__

#include "../app_handler_v2.h"
#include "../web_socket_res_process.h"
#include "../web_socket_data_process.h"
#include <memory>

namespace myframe {

// Forward declaration
class WsApplicationDataProcess;

// ============================================================================
// WebSocket Application Adapter (Level 1)
// 将 web_socket_res_process 适配到 IApplicationHandler 接口
// ============================================================================

class WsApplicationAdapter : public web_socket_res_process {
public:
    WsApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

    virtual ~WsApplicationAdapter();

    // 工厂方法（供 UnifiedProtocolFactory 使用）
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

private:
    IApplicationHandler* _handler;
};

// ============================================================================
// WebSocket Application Data Process
// WebSocket 数据处理层，调用 IApplicationHandler::on_ws()
// ============================================================================

class WsApplicationDataProcess : public web_socket_data_process {
public:
    WsApplicationDataProcess(
        web_socket_process* process,
        IApplicationHandler* handler);

    virtual ~WsApplicationDataProcess();

    // web_socket_data_process 接口实现
    virtual void on_handshake_ok() override;
    virtual void msg_recv_finish() override;
    virtual void on_close() override;
    virtual void on_ping(const char op_code, const std::string& ping_data) override;

private:
    IApplicationHandler* _handler;
    bool _handshake_done;
};

} // namespace myframe

#endif // __WS_APPLICATION_ADAPTER_H__
