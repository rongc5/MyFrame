// MyFrame Unified Protocol Architecture - Factory
// Unified protocol registration and detection
#ifndef __UNIFIED_PROTOCOL_FACTORY_H__
#define __UNIFIED_PROTOCOL_FACTORY_H__

#include "factory_base.h"
#include "app_handler_v2.h"
#include "protocol_context.h"
#include "base_data_process.h"
#include "base_net_obj.h"
#include <functional>
#include <vector>
#include <memory>
#include <string>
#include <algorithm>

// Forward declarations
class base_net_thread;
class common_obj_container;
struct normal_msg;

namespace myframe {

// ============================================================================
// Unified Protocol Factory
// 统一协议工厂 - 支持三层架构的协议注册和检测
// ============================================================================

class UnifiedProtocolFactory : public IFactory {
public:
    // 协议检测函数类型
    using DetectFn = std::function<bool(const char*, size_t)>;

    // 协议创建函数类型
    using CreateFn = std::function<std::unique_ptr<::base_data_process>(std::shared_ptr<base_net_obj>)>;

    UnifiedProtocolFactory();
    virtual ~UnifiedProtocolFactory();

    // ========================================================================
    // Level 1: Application Handler 注册
    // 最简单的使用方式，只需实现 IApplicationHandler
    // ========================================================================

    // 注册 HTTP 处理器（Level 1）
    UnifiedProtocolFactory& register_http_handler(IApplicationHandler* handler);

    // 注册 WebSocket 处理器（Level 1）
    UnifiedProtocolFactory& register_ws_handler(IApplicationHandler* handler);

    // 注册二进制协议处理器（Level 1）
    // 需要提供协议检测函数
    UnifiedProtocolFactory& register_binary_handler(
        const std::string& name,
        DetectFn detect,
        IApplicationHandler* handler,
        int priority = 50);

    // ========================================================================
    // Level 2: Protocol Context Handler 注册
    // 提供协议细节访问，支持异步、流式、状态管理
    // ========================================================================

    // 注册 HTTP Context 处理器（Level 2）
    UnifiedProtocolFactory& register_http_context_handler(IProtocolHandler* handler);

    // 注册 WebSocket Context 处理器（Level 2）
    UnifiedProtocolFactory& register_ws_context_handler(IProtocolHandler* handler);

    // 注册二进制协议 Context 处理器（Level 2）
    UnifiedProtocolFactory& register_binary_context_handler(
        const std::string& name,
        DetectFn detect,
        IProtocolHandler* handler,
        int priority = 50);

    // ========================================================================
    // Level 3: Custom Data Process 注册
    // 完全控制，直接注册 base_data_process 子类
    // ========================================================================

    // 注册自定义协议处理器（Level 3）
    UnifiedProtocolFactory& register_custom_protocol(
        const std::string& name,
        DetectFn detect,
        CreateFn create,
        int priority = 100);

    // 便捷方法：使用类的静态方法注册
    // 要求类有 static bool can_handle(const char*, size_t)
    // 和 static std::unique_ptr<base_data_process> create(shared_ptr<base_net_obj>)
    template<typename ProcessClass>
    UnifiedProtocolFactory& register_custom_protocol_class(
        const std::string& name,
        int priority = 100)
    {
        DetectFn detect = ProcessClass::can_handle;
        CreateFn create = ProcessClass::create;
        return register_custom_protocol(name, detect, create, priority);
    }

    // ========================================================================
    // IFactory 接口实现
    // ========================================================================

    void net_thread_init(base_net_thread* th);
    void handle_thread_msg(std::shared_ptr<::normal_msg>& msg);
    void register_worker(uint32_t thread_index);
    void on_accept(base_net_thread* listen_th, int fd);

    // Allows callers to disable the Level-3 detector when the listener handles a single plain protocol.
    void set_detector_enabled(bool enabled) { _detector_enabled = enabled; }
    bool detector_enabled() const { return _detector_enabled; }

    // ========================================================================
    // 查询接口
    // ========================================================================

    size_t protocol_count() const { return _protocols.size(); }

    // 获取注册的协议名称列表
    std::vector<std::string> get_protocol_names() const;

    // ========================================================================
    // 协议条目（公开给 ProtocolDetector 使用）
    // ========================================================================
    struct ProtocolEntry {
        std::string name;
        DetectFn detect;
        CreateFn create;
        int priority;  // 数字越小优先级越高
        bool terminal; // Whether selecting this entry finalizes protocol detection

        ProtocolEntry(const std::string& n, DetectFn d, CreateFn c, int p, bool term = true)
            : name(n), detect(d), create(c), priority(p), terminal(term) {}

        bool operator<(const ProtocolEntry& other) const {
            return priority < other.priority;
        }
    };

private:

    // 内部注册方法
    UnifiedProtocolFactory& register_protocol(
        const std::string& name,
        DetectFn detect,
        CreateFn create,
        int priority);

    // 成员变量
    std::vector<ProtocolEntry> _protocols;     // 注册的协议列表
    std::vector<uint32_t> _worker_indices;     // worker 线程索引
    uint32_t _rr_hint;                         // round-robin 提示
    common_obj_container* _container;          // 网络容器
    bool _detector_enabled;
};

} // namespace myframe

#endif // __UNIFIED_PROTOCOL_FACTORY_H__
