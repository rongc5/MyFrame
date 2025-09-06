#ifndef __MULTI_PROTOCOL_FACTORY_H__
#define __MULTI_PROTOCOL_FACTORY_H__

#include "factory_base.h"
#include "app_handler.h"
#include <memory>

// Forward declarations
class base_net_thread;
struct normal_msg;
class common_obj_container;

// Multi-protocol factory implementation
class MultiProtocolFactory : public IFactory {
public:
    enum class Mode {
        Auto,      // 自动检测协议
        TlsOnly,   // 只支持TLS
        PlainOnly  // 只支持明文
    };
    
    MultiProtocolFactory(IAppHandler* handler, Mode mode = Mode::Auto)
        : _handler(handler), _app_handler(handler), _mode(mode), _container(nullptr), _rr_hint(0) {}
    
    virtual ~MultiProtocolFactory() = default;
    
    // Accessors needed by server.cpp
    IAppHandler* handler() const { return _handler; }
    Mode mode() const { return _mode; }
    
    // IFactory interface - declarations only, implementations in multi_protocol_factory_impl.cpp
    void net_thread_init(base_net_thread* th) override;
    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override;
    void register_worker(uint32_t thread_index) override;
    void on_accept(base_net_thread* listen_th, int fd) override;
    
    // Container management needed by impl
    common_obj_container* _container;
    
private:
    IAppHandler* _handler;
    IAppHandler* _app_handler;  // For compatibility with impl
    Mode _mode;
    std::vector<uint32_t> _worker_indices;
    uint32_t _rr_hint;
};

#endif // __MULTI_PROTOCOL_FACTORY_H__