// MyFrame Unified Protocol Architecture - HTTP Level 1 Adapter
// Adapts http_res_process to IApplicationHandler interface
#ifndef __HTTP_APPLICATION_ADAPTER_H__
#define __HTTP_APPLICATION_ADAPTER_H__

#include "../app_handler_v2.h"
#include "../http_res_process.h"
#include "../http_base_data_process.h"
#include <memory>

namespace myframe {

// Forward declaration
class HttpApplicationDataProcess;

// ============================================================================
// HTTP Application Adapter (Level 1)
// 将 http_res_process 适配到 IApplicationHandler 接口
// ============================================================================

class HttpApplicationAdapter : public http_res_process {
public:
    HttpApplicationAdapter(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

    virtual ~HttpApplicationAdapter();

    // 工厂方法（供 UnifiedProtocolFactory 使用）
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IApplicationHandler* handler);

private:
    IApplicationHandler* _handler;
};

// ============================================================================
// HTTP Application Data Process
// HTTP 数据处理层，调用 IApplicationHandler::on_http()
// ============================================================================

class HttpApplicationDataProcess : public http_base_data_process {
public:
    HttpApplicationDataProcess(
        http_base_process* process,
        IApplicationHandler* handler);

    virtual ~HttpApplicationDataProcess();

    // http_base_data_process 接口实现
    virtual void msg_recv_finish() override;
    virtual std::string* get_send_head() override;
    virtual std::string* get_send_body(int& result) override;

private:
    IApplicationHandler* _handler;
    std::string _response_body;  // 保存响应体内容
    bool _body_sent;
};

} // namespace myframe

#endif // __HTTP_APPLICATION_ADAPTER_H__
