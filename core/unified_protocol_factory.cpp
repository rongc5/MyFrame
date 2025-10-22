// MyFrame Unified Protocol Architecture - Factory Implementation
#include "unified_protocol_factory.h"
#include "protocol_detector.h"
#include "protocol_adapters/http_application_adapter.h"
#include "protocol_adapters/ws_application_adapter.h"
#include "protocol_adapters/http_context_adapter.h"
#include "protocol_adapters/ws_context_adapter.h"
#include "protocol_adapters/binary_application_adapter.h"
#include "protocol_adapters/binary_context_adapter.h"
#include "common_def.h"
#include "common_obj_container.h"
#include "base_net_thread.h"
#include "base_connect.h"

namespace myframe {

UnifiedProtocolFactory::UnifiedProtocolFactory()
    : _rr_hint(0)
    , _container(nullptr)
{
}

UnifiedProtocolFactory::~UnifiedProtocolFactory() {
}

// ============================================================================
// Level 1: Application Handler 注册实现
// ============================================================================

UnifiedProtocolFactory& UnifiedProtocolFactory::register_http_handler(
    IApplicationHandler* handler)
{
    // HTTP 检测函数
    DetectFn detect = [](const char* buf, size_t len) -> bool {
        if (len < 4) return false;
        // 检测常见 HTTP 方法
        return (memcmp(buf, "GET ", 4) == 0 ||
                memcmp(buf, "POST ", 5) == 0 ||
                memcmp(buf, "PUT ", 4) == 0 ||
                memcmp(buf, "DELETE ", 7) == 0 ||
                memcmp(buf, "HEAD ", 5) == 0 ||
                memcmp(buf, "OPTIONS ", 8) == 0 ||
                memcmp(buf, "PATCH ", 6) == 0);
    };

    // 创建函数 - 创建 HttpApplicationAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return HttpApplicationAdapter::create(conn, handler);
    };

    return register_protocol("http", detect, create, 20);
}

UnifiedProtocolFactory& UnifiedProtocolFactory::register_ws_handler(
    IApplicationHandler* handler)
{
    // WebSocket 检测函数 - 必须先于 HTTP 检测
    DetectFn detect = [](const char* buf, size_t len) -> bool {
        if (len < 20) return false;  // 需要足够的数据
        std::string data(buf, std::min(len, size_t(512)));
        // 检测 WebSocket 升级请求（包含 Upgrade 和 websocket 关键字）
        return (data.find("Upgrade:") != std::string::npos &&
                (data.find("websocket") != std::string::npos ||
                 data.find("WebSocket") != std::string::npos));
    };

    // 创建函数 - 创建 WsApplicationAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return WsApplicationAdapter::create(conn, handler);
    };

    // 优先级 10，比 HTTP (20) 更高，确保 WebSocket 升级请求被正确识别
    return register_protocol("websocket", detect, create, 10);
}

UnifiedProtocolFactory& UnifiedProtocolFactory::register_binary_handler(
    const std::string& name,
    DetectFn detect,
    IApplicationHandler* handler,
    int priority)
{
    // 创建函数 - 创建 BinaryApplicationAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return std::unique_ptr<::base_data_process>(new BinaryApplicationAdapter(conn, handler));
    };

    return register_protocol(name, detect, create, priority);
}

// ============================================================================
// Level 2: Protocol Context Handler 注册实现
// ============================================================================

UnifiedProtocolFactory& UnifiedProtocolFactory::register_http_context_handler(
    IProtocolHandler* handler)
{
    // HTTP 检测函数
    DetectFn detect = [](const char* buf, size_t len) -> bool {
        if (len < 4) return false;
        // 检测常见 HTTP 方法
        return (memcmp(buf, "GET ", 4) == 0 ||
                memcmp(buf, "POST ", 5) == 0 ||
                memcmp(buf, "PUT ", 4) == 0 ||
                memcmp(buf, "DELETE ", 7) == 0 ||
                memcmp(buf, "HEAD ", 5) == 0 ||
                memcmp(buf, "OPTIONS ", 8) == 0 ||
                memcmp(buf, "PATCH ", 6) == 0);
    };

    // 创建函数 - 创建 HttpContextAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return HttpContextAdapter::create(conn, handler);
    };

    return register_protocol("http_ctx", detect, create, 20);
}

UnifiedProtocolFactory& UnifiedProtocolFactory::register_ws_context_handler(
    IProtocolHandler* handler)
{
    // WebSocket 检测函数 - 必须先于 HTTP 检测
    DetectFn detect = [](const char* buf, size_t len) -> bool {
        if (len < 20) return false;  // 需要足够的数据
        std::string data(buf, std::min(len, size_t(512)));
        // 检测 WebSocket 升级请求（包含 Upgrade 和 websocket 关键字）
        return (data.find("Upgrade:") != std::string::npos &&
                (data.find("websocket") != std::string::npos ||
                 data.find("WebSocket") != std::string::npos));
    };

    // 创建函数 - 创建 WsContextAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return WsContextAdapter::create(conn, handler);
    };

    // 优先级 10，比 HTTP (20) 更高，确保 WebSocket 升级请求被正确识别
    return register_protocol("websocket_ctx", detect, create, 10);
}

UnifiedProtocolFactory& UnifiedProtocolFactory::register_binary_context_handler(
    const std::string& name,
    DetectFn detect,
    IProtocolHandler* handler,
    int priority)
{
    // 创建函数 - 创建 BinaryContextAdapter
    CreateFn create = [handler](std::shared_ptr<base_net_obj> conn) -> std::unique_ptr<::base_data_process> {
        return BinaryContextAdapter::create(conn, handler);
    };

    return register_protocol(name, detect, create, priority);
}

// ============================================================================
// Level 3: Custom Protocol 注册实现
// ============================================================================

UnifiedProtocolFactory& UnifiedProtocolFactory::register_custom_protocol(
    const std::string& name,
    DetectFn detect,
    CreateFn create,
    int priority)
{
    return register_protocol(name, detect, create, priority);
}

// ============================================================================
// 内部方法
// ============================================================================

UnifiedProtocolFactory& UnifiedProtocolFactory::register_protocol(
    const std::string& name,
    DetectFn detect,
    CreateFn create,
    int priority)
{
    // 检查是否已注册
    for (const auto& proto : _protocols) {
        if (proto.name == name) {
            PDEBUG("[UnifiedFactory] Protocol '%s' already registered, skipping",
                   name.c_str());
            return *this;
        }
    }

    // 添加协议
    _protocols.emplace_back(name, detect, create, priority);

    // 按优先级排序
    std::sort(_protocols.begin(), _protocols.end());

    PDEBUG("[UnifiedFactory] Registered protocol '%s' with priority %d",
           name.c_str(), priority);

    return *this;
}

std::vector<std::string> UnifiedProtocolFactory::get_protocol_names() const {
    std::vector<std::string> names;
    names.reserve(_protocols.size());
    for (const auto& proto : _protocols) {
        names.push_back(proto.name);
    }
    return names;
}

// ============================================================================
// IFactory 接口实现
// ============================================================================

void UnifiedProtocolFactory::net_thread_init(base_net_thread* th) {
    if (!th) return;
    _container = th->get_net_container();
    PDEBUG("[UnifiedFactory] Thread %u initialized, %zu protocols registered",
           th->get_thread_index(), _protocols.size());
}

void UnifiedProtocolFactory::handle_thread_msg(std::shared_ptr<::normal_msg>& msg) {
    if (!msg || msg->_msg_op != NORMAL_MSG_CONNECT) return;

    PDEBUG("[UnifiedFactory] NORMAL_MSG_CONNECT received");

    if (!_container) {
        PDEBUG("[UnifiedFactory] Container is null");
        return;
    }

    std::shared_ptr<content_msg> cm = std::static_pointer_cast<content_msg>(msg);
    int fd = cm->fd;

    PDEBUG("[UnifiedFactory] Creating ProtocolDetector for fd=%d", fd);

    // 创建连接对象
    std::shared_ptr<base_connect<base_data_process>> conn(
        new base_connect<base_data_process>(fd));

    // 创建协议检测器
    // 需要将 ProtocolEntry 转换为 ProtocolDetector 可用的格式
    std::vector<ProtocolEntry> detector_protocols;
    detector_protocols.reserve(_protocols.size());
    for (const auto& proto : _protocols) {
        detector_protocols.push_back(proto);
    }

    std::unique_ptr<ProtocolDetector> detector(
        new ProtocolDetector(conn, detector_protocols));

    // 设置到连接
    conn->set_process(detector.release());
    conn->set_net_container(_container);

    // 加入容器
    std::shared_ptr<base_net_obj> net_obj = conn;
    _container->push_real_net(net_obj);

    PDEBUG("[UnifiedFactory] ProtocolDetector enqueued, thread=%u",
           _container->get_thread_index());
}

void UnifiedProtocolFactory::register_worker(uint32_t thread_index) {
    _worker_indices.push_back(thread_index);
    PDEBUG("[UnifiedFactory] Registered worker thread %u", thread_index);
}

void UnifiedProtocolFactory::on_accept(base_net_thread* listen_th, int fd) {
    if (!listen_th) return;

    uint32_t target_index = listen_th->get_thread_index();

    // Round-robin 到 worker 线程
    if (!_worker_indices.empty()) {
        unsigned long idx = _rr_hint++;
        target_index = _worker_indices[idx % _worker_indices.size()];
    }

    // 发送消息到目标线程
    std::shared_ptr<content_msg> cm(new content_msg());
    cm->fd = fd;
    std::shared_ptr<normal_msg> ng = std::static_pointer_cast<normal_msg>(cm);

    ObjId id;
    id._id = OBJ_ID_THREAD;
    id._thread_index = target_index;

    base_net_thread::put_obj_msg(id, ng);

    PDEBUG("[UnifiedFactory] Accepted fd=%d, dispatched to thread %u",
           fd, target_index);
}

} // namespace myframe
