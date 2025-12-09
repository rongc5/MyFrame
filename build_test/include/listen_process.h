#ifndef __LISTEN_PROCESS_H__
#define __LISTEN_PROCESS_H__

#include "common_def.h"
#include "base_net_thread.h"
#include "factory_base.h"
#include "base_net_obj.h"
#include <atomic>

class listen_process
{
    public:
        listen_process(std::shared_ptr<base_net_obj> p)
        {
            _listen_thread = NULL;
            _p_connect = p;
        }

        ~listen_process(){}

        void process(int fd)
        {
            // 优先使用监听线程维护的线程池进行分发
            if (_biz_factory && _listen_thread && _listen_thread->worker_count() > 0) {
                auto msg = std::make_shared<content_msg>();
                msg->fd = fd;
                auto ng = std::static_pointer_cast<normal_msg>(msg);
                ng->_msg_op = NORMAL_MSG_CONNECT;
                ObjId id; id._id = OBJ_ID_THREAD; id._thread_index = _listen_thread->next_worker_rr();
                base_net_thread::put_obj_msg(id, ng);
                return;
            }
            // 其次使用内部维护的 worker 列表（兼容旧接口）
            if (_biz_factory && !_worker_thd_vec.empty()) {
                auto msg = std::make_shared<content_msg>();
                msg->fd = fd;
                auto ng = std::static_pointer_cast<normal_msg>(msg);
                ng->_msg_op = NORMAL_MSG_CONNECT;
                size_t idx = _next.fetch_add(1) % _worker_thd_vec.size();
                ObjId id; id._id = OBJ_ID_THREAD; id._thread_index = _worker_thd_vec[idx];
                base_net_thread::put_obj_msg(id, ng);
                return;
            }

            // 兼容旧行为：回退到监听线程工厂的 on_accept
            if (_listen_thread && _listen_thread->get_factory()) {
                _listen_thread->get_factory()->on_accept(_listen_thread, fd);
                return;
            }

            // 最后兜底：直接按线程 hash 投递
            std::shared_ptr<content_msg>  net_obj(new content_msg);
            net_obj->fd = fd;
            ObjId id; id._id = OBJ_ID_THREAD;
            if (_worker_thd_vec.size()) {
                int index = (unsigned long) net_obj.get() % _worker_thd_vec.size();
                id._thread_index = _worker_thd_vec[index]; 
            } else if (_listen_thread) {
                id._thread_index = _listen_thread->get_thread_index(); 
            }
            std::shared_ptr<normal_msg> ng = std::static_pointer_cast<normal_msg>(net_obj);
            base_net_thread::put_obj_msg(id, ng);
        }

        void add_worker_thread(uint32_t thread_index)
        {
            _worker_thd_vec.push_back(thread_index);
        }

        void set_listen_thread(base_net_thread * thread)
        {
            _listen_thread = thread;
        }

        base_net_thread * get_listen_thread()
        {
            return _listen_thread;
        }

        // 下发业务工厂（用于 accept 后的轮询分发）
        void set_business_factory(IFactory* f) { _biz_factory = f; }

    protected:	
        std::weak_ptr<base_net_obj> _p_connect;
        base_net_thread * _listen_thread;
        std::vector<uint32_t> _worker_thd_vec;
        std::atomic<size_t> _next{0};
        IFactory* _biz_factory{nullptr};
};

#endif
