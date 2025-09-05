// Multi-protocol connection factory public API
#pragma once

#include "core/app_handler.h"
#include "factory_base.h"
#include <vector>
#include <cstdint>

class base_net_thread;
class common_obj_container;
struct normal_msg;

class MultiProtocolFactory : public IFactory {
public:
    enum class Mode { Auto, TlsOnly, PlainOnly };

    explicit MultiProtocolFactory(IAppHandler* h, Mode m = Mode::Auto)
        : _app_handler(h), _mode(m) {}

    // Accessors used by server to clone per-thread factories
    IAppHandler* handler() const { return _app_handler; }
    Mode mode() const { return _mode; }

    // IFactory overrides (implemented in core/multi_protocol_factory_impl.cpp)
    void net_thread_init(base_net_thread* th) override;
    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override;
    void register_worker(uint32_t thread_index) override;
    void on_accept(base_net_thread* listen_th, int fd) override;

private:
    friend class MultiProtocolFactoryImplAccessor;
    IAppHandler* _app_handler{nullptr};
    Mode _mode{Mode::Auto};
    std::vector<uint32_t> _worker_indices;
    unsigned long _rr_hint{0};
    common_obj_container* _container{nullptr};
};

