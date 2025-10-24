#include "ssl_context.h"
#include <atomic>
#include <memory>

// C++11 std::atomic<std::shared_ptr<T>> 支持检查
// GCC 5+, Clang 3.5+, MSVC 2015+ 支持通过全局函数操作
#if defined(__GNUC__) && !defined(__clang__)
    #if __GNUC__ < 5
        #warning "GCC 5+ recommended for std::atomic<std::shared_ptr<T>> support"
    #endif
#endif

namespace {

// TLS 配置快照 - 不可变对象，通过原子指针替换实现无锁读取
struct TlsRuntimeSnapshot {
    ssl_config server_conf;
    ssl_config client_conf;
    bool has_server;
    bool has_client;

    TlsRuntimeSnapshot()
        : server_conf(), client_conf(), has_server(false), has_client(false) {}
};

// 全局配置快照指针（通过 shared_ptr 自动管理内存生命周期）
std::shared_ptr<TlsRuntimeSnapshot> g_tls_snapshot(new TlsRuntimeSnapshot());

} // namespace

static std::shared_ptr<TlsRuntimeSnapshot> load_snapshot() {
    return std::atomic_load_explicit(&g_tls_snapshot, std::memory_order_acquire);
}

void tls_set_server_config(const ssl_config& conf) {
    std::shared_ptr<TlsRuntimeSnapshot> expected = load_snapshot();
    while (true) {
        std::shared_ptr<TlsRuntimeSnapshot> next(new TlsRuntimeSnapshot(*expected));
        next->server_conf = conf;
        next->has_server = true;
        if (std::atomic_compare_exchange_weak_explicit(
                &g_tls_snapshot,
                &expected,
                next,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return;
        }
        // compare_exchange 更新了 expected -> 重新基于最新快照构造
    }
}

bool tls_get_server_config(ssl_config& out) {
    std::shared_ptr<TlsRuntimeSnapshot> snapshot = load_snapshot();
    if (!snapshot->has_server) {
        return false;
    }
    out = snapshot->server_conf;
    return true;
}

void tls_set_client_config(const ssl_config& conf) {
    std::shared_ptr<TlsRuntimeSnapshot> expected = load_snapshot();
    while (true) {
        std::shared_ptr<TlsRuntimeSnapshot> next(new TlsRuntimeSnapshot(*expected));
        next->client_conf = conf;
        next->has_client = true;
        if (std::atomic_compare_exchange_weak_explicit(
                &g_tls_snapshot,
                &expected,
                next,
                std::memory_order_release,
                std::memory_order_acquire)) {
            return;
        }
    }
}

bool tls_get_client_config(ssl_config& out) {
    std::shared_ptr<TlsRuntimeSnapshot> snapshot = load_snapshot();
    if (!snapshot->has_client) {
        return false;
    }
    out = snapshot->client_conf;
    return true;
}

void tls_reset_runtime() {
    // 注意：此函数通常仅在测试或启动前调用
    // 运行时调用可能导致正在读取旧快照的线程看到空配置
    // 如果需要真正的线程安全重置，需要使用 RCU 或 hazard pointer 等技术
    std::shared_ptr<TlsRuntimeSnapshot> blank(new TlsRuntimeSnapshot());
    std::atomic_store_explicit(&g_tls_snapshot, blank, std::memory_order_release);
}
