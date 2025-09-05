#include "../core/client_conn_router.h"
#include "../core/base_net_thread.h"
#include <iostream>

int main(int argc, char** argv) {
    std::string url = (argc>1)?argv[1]:"cstm://127.0.0.1:7790/";
    std::string payload = (argc>2)?argv[2]:"CSTMhello"; // 前缀 CSTM 触发 CustomProbe

    base_net_thread th; th.start();
    ClientConnRouter router; register_default_client_builders(router);
    ClientBuildCtx ctx; ctx.container = th.get_net_container();
    // 使用 http builder 发送原始数据不合适；自定义协议可以复用 ws/http 管道或另行实现 builder。
    // 这里演示：直接用 ws 明文通道发送文本到服务端（也可新增自定义 builder）。
    std::shared_ptr<base_net_obj> net = router.create_and_enqueue("ws://127.0.0.1:7790/", ctx);
    if (!net) { std::cerr << "no builder for url: " << url << "\n"; return 2; }
    // 简化示例：仅驱动事件循环，由业务 handler 在服务端回显。
    th.join_thread();
    return 0;
}


