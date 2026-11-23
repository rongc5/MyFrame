// Per-thread network-thread identity used to enforce the owner-thread-only
// socket I/O invariant. Implemented in base_net_thread.cpp.
#ifndef __MYFRAME_NET_THREAD_CONTEXT_H__
#define __MYFRAME_NET_THREAD_CONTEXT_H__

#include <cstdint>

namespace myframe {
namespace detail {

// Thread index of the network thread whose event loop (or container teardown)
// is executing on the calling thread; 0 when the caller is not a network
// thread (application workers, TaskExecutor threads, main thread, ...).
uint32_t current_net_thread_index();

enum class NetThreadState {
    Gone,     // not registered (never created or already destroyed)
    Idle,     // registered, event loop not started yet
    Running,  // event loop running
    Stopped,  // registered, event loop has exited (awaiting destruction)
};

// While Idle there is no concurrent owner, so setup code may touch the
// connection directly. Stopped and Gone never allow direct access from a
// foreign thread: the container may be mid-teardown or already freed.
NetThreadState net_thread_state(uint32_t thread_index);

// RAII guard that marks the calling thread as the owner of thread_index.
class NetThreadScope {
public:
    explicit NetThreadScope(uint32_t thread_index);
    ~NetThreadScope();
    NetThreadScope(const NetThreadScope&) = delete;
    NetThreadScope& operator=(const NetThreadScope&) = delete;
private:
    uint32_t _prev;
};

} // namespace detail
} // namespace myframe

#endif
