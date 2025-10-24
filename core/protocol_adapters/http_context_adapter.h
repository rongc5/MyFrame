// MyFrame Unified Protocol Architecture - HTTP Level 2 Context Adapter
// Implements HttpContext for protocol-level control
#ifndef __HTTP_CONTEXT_ADAPTER_H__
#define __HTTP_CONTEXT_ADAPTER_H__

#include "../protocol_context.h"
#include "../http_res_process.h"
#include "../http_base_data_process.h"
#include <memory>
#include <map>
#include <thread>
#include <queue>

namespace myframe {

// Forward declarations
class HttpContextDataProcess;

// ============================================================================
// HTTP Context Implementation
// �ṩЭ��ϸ�ڷ��ʺͿ���
// ============================================================================

class HttpContextImpl : public HttpContext {
public:
    HttpContextImpl(http_base_process* process, std::shared_ptr<base_net_obj> conn);
    virtual ~HttpContextImpl();

    // HttpContext �ӿ�ʵ��
    const HttpRequest& request() const override;
    HttpResponse& response() override;

    void async_response(std::function<void()> fn) override;
    void complete_async_response() override;
    void enable_streaming() override;
    size_t stream_write(const void* data, size_t len) override;
    void upgrade_to_websocket() override;

    void close() override;
    void set_timeout(uint64_t ms) override;
    void keep_alive(bool enable) override;

    void set_user_data(const std::string& key, void* data) override;
    void* get_user_data(const std::string& key) const override;

    ConnectionInfo& connection_info() override;
    std::shared_ptr<base_net_obj> raw_connection() override;

    base_net_thread* get_thread() const override;

    // Timer and message management
    void add_timer(uint64_t timeout_ms, std::shared_ptr<::timer_msg> t_msg) override;
    void send_msg(std::shared_ptr<::normal_msg> msg) override;

    // ���� HttpContextDataProcess ����˽�г�Ա
    friend class HttpContextDataProcess;

private:
    http_base_process* _process;
    std::shared_ptr<base_net_obj> _conn;
    HttpRequest _request;
    HttpResponse _response;
    std::map<std::string, void*> _user_data;
    ConnectionInfo _conn_info;
    bool _streaming_enabled;
};

// ============================================================================
// HTTP Context Adapter (Level 2)
// �� http_res_process ���䵽 IProtocolHandler �ӿ�
// ============================================================================

class HttpContextAdapter : public http_res_process {
public:
    HttpContextAdapter(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

    virtual ~HttpContextAdapter();

    // ������������ UnifiedProtocolFactory ʹ�ã�
    static std::unique_ptr<base_data_process> create(
        std::shared_ptr<base_net_obj> conn,
        IProtocolHandler* handler);

private:
    IProtocolHandler* _handler;
};

// ============================================================================
// HTTP Context Data Process
// HTTP ���ݴ����㣬���� IProtocolHandler::on_http_request()
// ============================================================================

class HttpContextDataProcess : public http_base_data_process {
public:
    HttpContextDataProcess(
        http_base_process* process,
        IProtocolHandler* handler);

    virtual ~HttpContextDataProcess();

    // ��д�ؼ��麯��
    size_t process_recv_body(const char* buf, size_t len, int& result) override;
    std::string* get_send_head() override;
    std::string* get_send_body(int& result) override;
    void msg_recv_finish() override;
    void complete_async_response() override;
    void handle_timeout(std::shared_ptr<::timer_msg>& t_msg) override;
    void handle_msg(std::shared_ptr<::normal_msg>& msg) override;

private:
    IProtocolHandler* _handler;
    std::shared_ptr<HttpContextImpl> _context;
    std::string _recv_body;   // �洢������
    std::string _send_body;   // �洢��Ӧ��
};

} // namespace myframe

#endif // __HTTP_CONTEXT_ADAPTER_H__
