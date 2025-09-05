#pragma once

#include <memory>
#include <string>
#include <vector>
#include "listen_connect.h"
#include "listen_process.h"
#include "multi_protocol_factory.h"
#include "base_net_thread.h"
#include "factory_base.h"

// ListenFactory：实现 IFactory，可直接用于 server.set_business_factory()
class ListenFactory : public IFactory {
public:
    ListenFactory(const std::string& ip, unsigned short port,
                  std::shared_ptr<IFactory> business)
        : _ip(ip), _port(port), _biz_factory(std::move(business)) {}

    // IFactory: 线程初始化时创建监听对象并注册到 epoll
    void net_thread_init(base_net_thread* th) override {
        if (!th) return;
        bool expected = false;
        if (!_initialized.compare_exchange_strong(expected, true)) {
            return; // 已初始化（防并发二次初始化）
        }

        std::shared_ptr< listen_connect<listen_process> > conn;
        listen_process* proc = nullptr;
        _listen_fd = init_listen(th, _ip, _port, conn, proc);
        _listen_conn = std::move(conn);
        _listen_proc.reset(proc);

        // 绑定业务工厂，并将已注册的 worker 交给 listen_process 做轮询
        if (_listen_proc) {
            _listen_proc->set_business_factory(_biz_factory.get());
            for (auto idx : _worker_indices) _listen_proc->add_worker_thread(idx);
        }
    }

    void handle_thread_msg(std::shared_ptr<normal_msg>& msg) override {
        if (_biz_factory) _biz_factory->handle_thread_msg(msg);
    }

    void register_worker(uint32_t thread_index) override {
        _worker_indices.push_back(thread_index);
        if (_listen_proc) _listen_proc->add_worker_thread(thread_index);
    }

    void on_accept(base_net_thread* listen_th, int fd) override {
        // 兼容路径：若 listen_process 未接管分发，直接转交业务工厂
        if (_biz_factory) _biz_factory->on_accept(listen_th, fd);
    }

    IFactory* inner_factory() const { return _biz_factory.get(); }

    // Initialize listen socket, attach to container/epoll, and return fd
    static int init_listen(base_net_thread* owner_thread,
                           const std::string& ip, unsigned short port,
                           std::shared_ptr< listen_connect<listen_process> >& out_conn,
                           listen_process*& out_proc)
    {
        out_conn.reset(new listen_connect<listen_process>(ip, port));
        int fd = out_conn ? out_conn->get_sfd() : -1;
        if (fd < 0) {
            std::fprintf(stderr, "[listen_factory] invalid listen fd for %s:%u\n", ip.c_str(), port);
            throw std::runtime_error("listen failed");
        }

        out_proc = new listen_process(out_conn);
        out_proc->set_listen_thread(owner_thread);
        out_conn->set_process(out_proc);
        // 注册到 epoll
        out_conn->set_net_container(owner_thread->get_net_container());
        // 改为边沿触发，避免饥饿
        out_conn->update_event(EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLET);
        std::fprintf(stderr, "[listen] added listen_fd=%d to epoll\n", fd);
        return fd;
    }

private:
    std::string _ip; unsigned short _port{0};
    int _listen_fd{-1};
    std::atomic<bool> _initialized{false};

    std::shared_ptr<IFactory> _biz_factory;    // 业务工厂（如 MultiProtocolFactory）
    std::vector<uint32_t> _worker_indices;     // 监听侧保存的 worker 列表

    // 保存 listen 的连接对象，防止被容器析构
    std::shared_ptr< listen_connect<listen_process> > _listen_conn;
    std::unique_ptr<listen_process>                 _listen_proc;
};
