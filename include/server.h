// Public server facade used by tests/examples
#pragma once

#include <memory>
#include <string>
#include <vector>

class base_net_thread;
class IThreadPlugin;
class IFactory;
class MultiProtocolFactory;

class server {
public:
    explicit server(int thread_num = 2);
    ~server();

    void bind(const std::string& ip, unsigned short port);
    void set_business_factory(std::shared_ptr<IFactory> factory);

    void start();
    void stop();
    void join();

    size_t size() const;
    void add_plugin(std::shared_ptr<IThreadPlugin> p);

private:
    int _threads{1};
    std::string _ip;
    unsigned short _port{0};

    std::vector<base_net_thread*> _workers;
    base_net_thread* _listen{nullptr};

    std::shared_ptr<IFactory> _factory;
    std::shared_ptr<IFactory> _acceptor;
    std::vector<std::shared_ptr<IThreadPlugin>> _plugins;
    // Keep per-worker cloned factories (for multi-protocol mode)
    std::vector<std::shared_ptr<MultiProtocolFactory>> _worker_factories;
};
