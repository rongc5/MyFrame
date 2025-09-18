#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

class app_ws_data_process;

// 简单的基于用户名的WS连接Hub（MyFrame版）
class WsPushHub {
public:
    static WsPushHub& Instance() {
        static WsPushHub hub; return hub;
    }

    void Register(const std::string& user, app_ws_data_process* proc) {
        if (user.empty() || !proc) return;
        std::lock_guard<std::mutex> lk(mu_);
        users_[user].insert(proc);
        rev_[proc] = user;
    }

    void Unregister(app_ws_data_process* proc) {
        if (!proc) return;
        std::lock_guard<std::mutex> lk(mu_);
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

    void BroadcastToUser(const std::string& user, const std::string& payload);
    void BroadcastAll(const std::string& payload);

private:
    WsPushHub() = default;
    std::unordered_map<std::string, std::unordered_set<app_ws_data_process*>> users_;
    std::unordered_map<app_ws_data_process*, std::string> rev_;
    std::mutex mu_;
};
