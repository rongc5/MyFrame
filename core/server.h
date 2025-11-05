#ifndef __SERVER_H__
#define __SERVER_H__

#include <string>
#include <vector>
#include <memory>

// Forward declarations
class base_net_thread;
class IFactory;
class ListenFactory;
class MultiProtocolFactory;
class IThreadPlugin;

class server {
public:
    server(int thread_num = 1);
    virtual ~server();
    
    // 绑定地址和端口
    void bind(const std::string& ip, unsigned short port);
    
    // 设置业务工厂
    void set_business_factory(std::shared_ptr<IFactory> factory);
    
    // 启动服务器
    void start();
    
    // 停止服务器
    void stop();
    
    // 等待线程结束
    void join();
    
    // 添加线程插件
    void add_plugin(std::shared_ptr<IThreadPlugin> plugin);
    
    // 获取线程数量
    size_t size() const;

    // Expose worker thread instances without transferring ownership
    const std::vector<base_net_thread*>& worker_threads() const;
    base_net_thread* listen_thread() const;
    
private:
    int _threads;
    std::string _ip;
    unsigned short _port;
    
    std::shared_ptr<IFactory> _factory;
    std::shared_ptr<IFactory> _acceptor;
    std::vector<std::shared_ptr<MultiProtocolFactory>> _worker_factories;
    std::vector<base_net_thread*> _workers;
    base_net_thread* _listen;
    
    std::vector<std::shared_ptr<IThreadPlugin>> _plugins;
};

#endif // __SERVER_H__
