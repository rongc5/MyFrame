#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include "rwlock.h"
#include "web_socket_data_process.h"

class app_ws_data_process;

// Username-keyed WebSocket push hub.
//
// Thread-safety / lifetime model:
//   * Register()/Unregister() are called by app_ws_data_process on the
//     connection's owner thread (handshake completion / close / destruction).
//   * BroadcastToUser()/BroadcastAll() may be called from any thread. They
//     never dereference an app_ws_data_process pointer; each registration is
//     stored as a ws_send_handle (ObjId + generation + liveness token) and the
//     payload is delivered through base_net_thread::put_obj_msg() to the
//     owner thread, where a closed connection simply drops the message.
//   * Every delivery is reserved against the target's outbound high-water
//     mark first; a connection that stops reading is closed as a slow
//     consumer instead of buffering without bound.
class WsPushHub {
public:
    static WsPushHub& Instance() {
        static WsPushHub hub; return hub;
    }

    // Must be called on proc's owner thread (app_ws_data_process does this).
    void Register(const std::string& user, app_ws_data_process* proc);
    // proc is used as an opaque key only; it is never dereferenced.
    void Unregister(app_ws_data_process* proc);

    void BroadcastToUser(const std::string& user, const std::string& payload);
    void BroadcastAll(const std::string& payload);

    // Same as above, returning the number of connections the payload was
    // accepted (reserved + posted) for.
    size_t BroadcastToUserCounted(const std::string& user, const std::string& payload);
    size_t BroadcastAllCounted(const std::string& payload);

    size_t ConnectionCount() const;
    size_t UserConnectionCount(const std::string& user) const;

private:
    WsPushHub() = default;
    typedef std::unordered_map<const app_ws_data_process*, ws_send_handle> HandleMap;
    std::unordered_map<std::string, HandleMap> users_;
    std::unordered_map<const app_ws_data_process*, std::string> rev_;
    mutable RWLock rwlock_;
};
