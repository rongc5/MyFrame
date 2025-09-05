#pragma once

#include <cstdint>

class base_net_thread;

class IThreadPlugin {
public:
    virtual const char* id() const = 0;
    virtual void on_init(base_net_thread* thread) = 0;
    virtual void on_tick() = 0;
    virtual void on_stop() {}
    virtual ~IThreadPlugin() {}
};

