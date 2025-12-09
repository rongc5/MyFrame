#ifndef __FACTORY_BASE_H__
#define __FACTORY_BASE_H__

#include <memory>
#include <cstdint>

// Forward declarations
class base_net_thread;
struct normal_msg;

// Base factory interface for MyFrame
class IFactory {
public:
    virtual ~IFactory() = default;
    
    // Thread lifecycle hooks
    virtual void net_thread_init(base_net_thread* th) {}
    virtual void handle_thread_msg(std::shared_ptr<normal_msg>& msg) {}
    virtual void register_worker(uint32_t thread_index) {}
    virtual void on_accept(base_net_thread* listen_th, int fd) {}
};

#endif // __FACTORY_BASE_H__