#include "ws_push_hub.h"
#include "app_ws_data_process.h"
#include <vector>

void WsPushHub::BroadcastToUser(const std::string& user, const std::string& payload) {
    std::unordered_set<app_ws_data_process*> conns;
    {
        ReadLockGuard lk(rwlock_);
        auto it = users_.find(user);
        if (it == users_.end()) return;
        conns = it->second; // copy
    }
    for (auto* p : conns) {
        if (p) p->send_text(payload);
    }
}

void WsPushHub::BroadcastAll(const std::string& payload) {
    std::vector<app_ws_data_process*> conns;
    {
        ReadLockGuard lk(rwlock_);
        for (auto& kv : users_) {
            for (auto* p : kv.second) conns.push_back(p);
        }
    }
    for (auto* p : conns) {
        if (p) p->send_text(payload);
    }
}
