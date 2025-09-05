#include "ssl_context.h"
#include <mutex>

namespace { std::mutex g_mu; bool g_has_server=false; ssl_config g_server_conf; bool g_has_client=false; ssl_config g_client_conf; }

void tls_set_server_config(const ssl_config& conf) { std::lock_guard<std::mutex> lk(g_mu); g_server_conf = conf; g_has_server = true; }
bool tls_get_server_config(ssl_config& out) { std::lock_guard<std::mutex> lk(g_mu); if (!g_has_server) return false; out = g_server_conf; return true; }
void tls_set_client_config(const ssl_config& conf) { std::lock_guard<std::mutex> lk(g_mu); g_client_conf = conf; g_has_client = true; }
bool tls_get_client_config(ssl_config& out) { std::lock_guard<std::mutex> lk(g_mu); if (!g_has_client) return false; out = g_client_conf; return true; }

void tls_reset_runtime() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_has_server = false;
    g_has_client = false;
    g_server_conf = ssl_config();
    g_client_conf = ssl_config();
}
