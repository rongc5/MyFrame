#include "ws_push_hub.h"
#include "app_ws_data_process.h"
#include <vector>

void WsPushHub::Register(const std::string& user, app_ws_data_process* proc) {
    if (user.empty() || !proc) return;
    // Called on the owner thread, so building the handle may touch proc.
    ws_send_handle h = proc->send_handle();
    if (!h.valid()) return;
    WriteLockGuard lk(rwlock_);
    auto old = rev_.find(proc);
    if (old != rev_.end()) {
        auto uit = users_.find(old->second);
        if (uit != users_.end()) {
            uit->second.erase(proc);
            if (uit->second.empty()) users_.erase(uit);
        }
        rev_.erase(old);
    }
    users_[user][proc] = h;
    rev_[proc] = user;
}

void WsPushHub::Unregister(app_ws_data_process* proc) {
    if (!proc) return;
    WriteLockGuard lk(rwlock_);
    auto it = rev_.find(proc);
    if (it != rev_.end()) {
        auto uit = users_.find(it->second);
        if (uit != users_.end()) {
            uit->second.erase(proc);
            if (uit->second.empty()) users_.erase(uit);
        }
        rev_.erase(it);
    }
}

size_t WsPushHub::BroadcastToUserCounted(const std::string& user, const std::string& payload) {
    std::vector<ws_send_handle> targets;
    {
        ReadLockGuard lk(rwlock_);
        auto it = users_.find(user);
        if (it == users_.end()) return 0;
        targets.reserve(it->second.size());
        for (const auto& kv : it->second) targets.push_back(kv.second);
    }
    std::shared_ptr<const std::string> shared = std::make_shared<const std::string>(payload);
    size_t accepted = 0;
    for (const auto& h : targets) {
        if (web_socket_data_process::post_send(h, shared, (int8_t)myframe::WsFrame::TEXT)) ++accepted;
    }
    return accepted;
}

size_t WsPushHub::BroadcastAllCounted(const std::string& payload) {
    std::vector<ws_send_handle> targets;
    {
        ReadLockGuard lk(rwlock_);
        targets.reserve(rev_.size());
        for (const auto& u : users_) {
            for (const auto& kv : u.second) targets.push_back(kv.second);
        }
    }
    std::shared_ptr<const std::string> shared = std::make_shared<const std::string>(payload);
    size_t accepted = 0;
    for (const auto& h : targets) {
        if (web_socket_data_process::post_send(h, shared, (int8_t)myframe::WsFrame::TEXT)) ++accepted;
    }
    return accepted;
}

void WsPushHub::BroadcastToUser(const std::string& user, const std::string& payload) {
    (void)BroadcastToUserCounted(user, payload);
}

void WsPushHub::BroadcastAll(const std::string& payload) {
    (void)BroadcastAllCounted(payload);
}

size_t WsPushHub::ConnectionCount() const {
    ReadLockGuard lk(rwlock_);
    return rev_.size();
}

size_t WsPushHub::UserConnectionCount(const std::string& user) const {
    ReadLockGuard lk(rwlock_);
    auto it = users_.find(user);
    return it == users_.end() ? 0 : it->second.size();
}
