// Base factory interface for net threads and acceptors
#pragma once

#include <cstdint>
#include <memory>

class base_net_thread;
struct normal_msg;

class IFactory {
public:
    virtual ~IFactory() = default;

    // Called after base_net_thread constructed
    virtual void net_thread_init(base_net_thread* /*th*/) {}

    // Dispatch messages sent to this thread
    virtual void handle_thread_msg(std::shared_ptr<normal_msg>& /*msg*/) {}

    // Register worker thread indices (used by acceptor/factories that need fan-out)
    virtual void register_worker(uint32_t /*thread_index*/) {}

    // Called by listen socket on new accepted fd
    virtual void on_accept(base_net_thread* /*listen_th*/, int /*fd*/) {}
};

