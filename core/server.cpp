#include "server.h"
#include "base_net_thread.h"
#include "listen_factory.h"
#include "base_thread.h"
#include "thread_plugin.h"
#include "factory_base.h"
#include "multi_protocol_factory.h"
#include <signal.h>

server::server(int thread_num)
    : _threads(thread_num > 0 ? thread_num : 1), _port(0), _listen(nullptr) {}

server::~server() {
    stop();
    join();
}

void server::bind(const std::string& ip, unsigned short port) {
    _ip = ip; _port = port;
}

void server::set_business_factory(std::shared_ptr<IFactory> factory) {
    _factory = factory;
}

void server::start() {
    // 防止任何写关闭触发的 SIGPIPE 终止进程
    signal(SIGPIPE, SIG_IGN);
    // 创建并启动 worker 线程（每个worker尽量拥有独立的业务工厂实例）
    int worker_count = _threads > 1 ? _threads - 1 : 0;
    for (int i = 0; i < worker_count; ++i) {
        IFactory* factory_for_thread = _factory.get();

        // 如果传入的是 ListenFactory，则提取其内部的业务工厂
        if (auto lsn = dynamic_cast<ListenFactory*>(factory_for_thread)) {
            factory_for_thread = lsn->inner_factory();
        }
        // 为每个worker克隆一个MultiProtocolFactory，避免跨线程共享容器指针
        if (auto mpf = dynamic_cast<MultiProtocolFactory*>(factory_for_thread)) {
            std::shared_ptr<MultiProtocolFactory> per_thread(new MultiProtocolFactory(mpf->handler(), mpf->mode()));
            _worker_factories.push_back(per_thread);
            factory_for_thread = per_thread.get();
        }
        base_net_thread* w = new base_net_thread(factory_for_thread);
        for (size_t pi = 0; pi < _plugins.size(); ++pi) { w->add_plugin(_plugins[pi]); }
        w->start();
        _workers.push_back(w);
    }

    // 创建并启动 listen 线程（直接使用业务 MultiProtocolFactory 作为分发者）
    _acceptor = _factory; // 监听线程使用传入工厂（可为 ListenFactory 包装器）
    // 将所有 worker 注册给监听线程的工厂（用于 round-robin 分发）
    for (auto* w : _workers) {
        if (_acceptor) _acceptor->register_worker(w->get_thread_index());
    }

    _listen = new base_net_thread(_acceptor.get());
    {
        // 如果业务工厂本身是 ListenFactory，则其 net_thread_init 会完成监听初始化
        // 否则，沿用旧路径由静态辅助函数初始化监听
        if (dynamic_cast<ListenFactory*>(_acceptor.get()) == nullptr) {
            std::shared_ptr< listen_connect<listen_process> > tmp_conn;
            listen_process* tmp_proc = nullptr;
            int listen_fd = ListenFactory::init_listen(_listen, _ip, _port, tmp_conn, tmp_proc);
            (void)tmp_proc; // 生命周期由 listen_connect 管理
            PDEBUG("[listen] bound %s:%u, fd=%d", _ip.c_str(), _port, listen_fd);
        }
    }
    // 将 worker 线程索引注入到监听线程的线程池中（供 listen_process 优先使用）
    for (auto* w : _workers) {
        if (_listen) _listen->add_worker_thread(w->get_thread_index());
    }
    // base_net_thread 构造中已调用 net_thread_init；无需重复
    for (size_t pi = 0; pi < _plugins.size(); ++pi) { _listen->add_plugin(_plugins[pi]); }
    _listen->start();
}

void server::stop() {
    base_thread::stop_all_thread();
}

void server::join() {
    base_thread::join_all_thread();
}

size_t server::size() const { return (size_t)_threads; }

const std::vector<base_net_thread*>& server::worker_threads() const { return _workers; }

base_net_thread* server::listen_thread() const { return _listen; }

void server::add_plugin(std::shared_ptr<IThreadPlugin> p) { _plugins.push_back(p); }
