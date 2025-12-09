#ifndef __BASE_NET_THREAD_H__
#define __BASE_NET_THREAD_H__

#include "common_def.h"
#include "base_thread.h"
#include "common_obj_container.h"
#include "base_connect.h"
#include "channel_data_process.h"
#include "thread_plugin.h"
#ifndef __LISTEN_FWD__
#define __LISTEN_FWD__
template <class PROCESS> class listen_connect;
class listen_process;
#endif
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

class IFactory; // fwd


class base_net_container;
class base_net_thread:public base_thread
{
    public:
        base_net_thread(int channel_num = 1);
        base_net_thread(class IFactory* factory, int channel_num = 1);
        virtual ~base_net_thread();

        virtual void *run();

        virtual void run_process();

        virtual void net_thread_init();

        virtual void put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg);

        static void put_obj_msg(ObjId & id, std::shared_ptr<normal_msg> & p_msg);

        virtual void handle_msg(std::shared_ptr<normal_msg> & p_msg);

        static base_net_thread * get_base_net_thread_obj(uint32_t thread_index);

        void add_timer(std::shared_ptr<timer_msg> & t_msg);

        virtual void handle_timeout(std::shared_ptr<timer_msg> & t_msg);

    common_obj_container * get_net_container();

    // 线程池/工作线程索引管理（供监听线程进行分发）
    void add_worker_thread(uint32_t thread_index);
    const std::vector<uint32_t>& workers() const { return _worker_pool; }
    size_t worker_count() const { return _worker_pool.size(); }
    uint32_t next_worker_rr();

    protected:

        virtual bool handle_thread_msg(std::shared_ptr<normal_msg> & p_msg);

        int _channel_num;
        std::vector< std::shared_ptr<base_connect<channel_data_process> > > _channel_msg_vec;

        common_obj_container * _base_container;

        static std::unordered_map<uint32_t, base_net_thread *> _base_net_thread_map;

        std::vector<std::shared_ptr<IThreadPlugin> > _plugins;
    public:
        void add_plugin(std::shared_ptr<IThreadPlugin> p) { _plugins.push_back(p); }
        void set_factory_for_thread(IFactory* f) { _factory_for_thread = f; }
        IFactory* get_factory() const { return _factory_for_thread; }

        // ===== Custom Thread Data =====
        // Stores a non-owning pointer; caller must guarantee the pointee outlives the entry.
        void set_user_data(const std::string& key, void* value) { set_user_data_unowned(key, value); }
        void set_user_data_unowned(const std::string& key, void* value);

        // Takes ownership; pointer must be allocated with new T.
        template<typename T>
        void set_user_data_owned(const std::string& key, T* value) {
            if (!value) {
                _user_data.erase(key);
                return;
            }
            store_user_data(key, std::shared_ptr<T>(value), true);
        }

        // Shares ownership via std::shared_ptr; reference counts stay in sync with callers.
        template<typename T>
        void set_user_data_shared(const std::string& key, std::shared_ptr<T> value) {
            if (!value) {
                _user_data.erase(key);
                return;
            }
            store_user_data(key, std::static_pointer_cast<void>(std::move(value)), true);
        }

        void set_user_data_shared(const std::string& key, std::shared_ptr<void> value) {
            if (!value) {
                _user_data.erase(key);
                return;
            }
            store_user_data(key, std::move(value), true);
        }

        template<typename T>
        T* get_user_data(const std::string& key) const {
            auto it = _user_data.find(key);
            if (it == _user_data.end() || !it->second.handle) {
                return nullptr;
            }
            return static_cast<T*>(it->second.handle.get());
        }

        // Attempts to fetch a shared_ptr when the entry owns the data; returns nullptr for unowned.
        template<typename T>
        std::shared_ptr<T> get_user_data_ptr(const std::string& key) const {
            auto it = _user_data.find(key);
            if (it == _user_data.end() || !it->second.handle || !it->second.owns) {
                return nullptr;
            }
            return std::static_pointer_cast<T>(it->second.handle);
        }

        // Returns the stored pointer regardless of ownership; may dangle for unowned entries.
        void* get_user_data(const std::string& key) const {
            return get_user_data<void>(key);
        }

        bool has_user_data(const std::string& key) const {
            return _user_data.find(key) != _user_data.end();
        }

        void clear_user_data(const std::string& key);
        void clear_user_data();
    protected:
        IFactory* _factory_for_thread{nullptr};
        std::vector<uint32_t> _worker_pool; // 监听线程可维护的 worker 索引集合
        unsigned long _rr_hint{0};
        struct ThreadUserDataEntry {
            std::shared_ptr<void> handle;
            bool owns{false};
        };
        std::unordered_map<std::string, ThreadUserDataEntry> _user_data;

        void store_user_data(const std::string& key, std::shared_ptr<void> value, bool owns);
};

#endif
