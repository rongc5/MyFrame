// channel watchdog 修复回归测试
//
// 验证两件事(对应 channel_data_process.cpp:78 和 common_obj_container.cpp:190 的修复):
//
// 1. channel_data_process::add_event_timer 注册的 NONE_CHANNEL_EVENT_TIMER_TYPE
//    timer 现在路由回 channel_data_process::handle_timeout，不再被
//    common_domain::handle_timeout 静默吃掉。
// 2. obj_process 不再用 wants_tick() 过滤 _obj_net_map 成员，所以
//    set_real_net(true) 推回来的 channel 真的会被 real_net_process()，
//    把 stuck 的 queue drain 出来。
//
// 联起来：watchdog 是把"消费者卡住、queue 堆积"的 channel 自愈回正常状态的
// 唯一兜底机制。修复前两件事都是死代码，channel 一旦异常脱离 epoll 永远回不来。

#include "../core/common_obj_container.h"
#include "../core/base_connect.h"
#include "../core/channel_data_process.h"
#include "../core/base_net_obj.h"
#include "../core/base_def.h"
#include "../core/common_epoll.h"
#include "../core/base_timer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// 记录 handle_timeout 被调用次数；其他逻辑沿用父类。
class probed_channel : public channel_data_process {
public:
    std::atomic<int> handle_timeout_calls{0};

    using channel_data_process::channel_data_process;

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        ++handle_timeout_calls;
        channel_data_process::handle_timeout(t_msg);
    }
};

// 测试用的接收端：被 process_recv_buf drain 出来的消息会经
// container.handle_msg → find(obj_id) → 这里 handle_msg。
class probe_handler : public base_net_obj {
public:
    std::atomic<int> handle_msg_calls{0};

    void handle_msg(std::shared_ptr<normal_msg>& /*p_msg*/) override {
        ++handle_msg_calls;
    }

    // base_net_obj 的两个纯虚：handler 不接管真实 fd 上的事件，桩成 no-op。
    void event_process(int /*events*/) override {}
    int  real_net_process() override { return 0; }
};

bool test1_timer_routing() {
    std::cout << "\n=== TEST 1: watchdog timer routes to channel_data_process::handle_timeout ===\n";

    common_obj_container container(99);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "FAIL: socketpair failed\n";
        return false;
    }

    auto conn = std::make_shared<base_connect<probed_channel>>(fds[0]);
    auto* probe = new probed_channel(conn, fds[1]);
    conn->set_process(probe);
    conn->set_net_container(&container);

    // 任何 net_obj 都要走 set_net_container 拿到容器分配的 obj_id；
    // 这里是接收 drain 出来的消息的目标。
    auto handler = std::make_shared<probe_handler>();
    handler->set_net_container(&container);
    uint32_t handler_id = handler->get_id()._id;

    // 第一次 process_recv_buf：queue 空 swap、_last_time 置位、首次 add_event_timer。
    char dummy_tag[2] = "c";
    probe->process_recv_buf(dummy_tag, 0);

    // 让 queue 长度 > 0，watchdog 触发条件之一。
    auto msg = std::make_shared<normal_msg>();
    probe->put_msg(handler_id, msg);

    // MAX_CHANNEL_EVENT_TIMEOUT = 1000ms，等过这点 watchdog 才会判 elapsed > _time_length。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    std::vector<uint32_t> exp_vec;
    container.get_timer()->check_timer(exp_vec);

    if (probe->handle_timeout_calls.load() == 0) {
        std::cerr << "FAIL: channel_data_process::handle_timeout was never invoked. "
                     "Watchdog timer is dead-routed (expected fix at "
                     "channel_data_process.cpp:78).\n";
        return false;
    }
    std::cout << "PASS: handle_timeout invoked "
              << probe->handle_timeout_calls.load() << " time(s)\n";
    return true;
}

bool test2_stuck_channel_recovery() {
    std::cout << "\n=== TEST 2: stuck channel auto-recovers via watchdog + obj_process ===\n";

    common_obj_container container(99);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "FAIL: socketpair failed\n";
        return false;
    }

    auto conn = std::make_shared<base_connect<probed_channel>>(fds[0]);
    auto* probe = new probed_channel(conn, fds[1]);
    conn->set_process(probe);
    conn->set_net_container(&container);

    auto handler = std::make_shared<probe_handler>();
    handler->set_net_container(&container);
    uint32_t handler_id = handler->get_id()._id;

    // 注册首个 watchdog 计时器并设 _last_time。
    char dummy_tag[2] = "c";
    probe->process_recv_buf(dummy_tag, 0);

    // 模拟生产事故现场：channel 的 fd 从 epoll 摘掉。这就是 5/5 09:05:55
    // 之后 ReloadThread channel 的状态——put_msg 的 tag 字节落进 socket
    // buffer，但 epoll 不再 fire EPOLLIN，consumer 不会被叫醒。
    container.get_epoll()->del_from_epoll(conn.get());

    // 生产者继续投递，3 条消息全在 queue 里堆着。
    constexpr int kStuck = 3;
    for (int i = 0; i < kStuck; ++i) {
        auto msg = std::make_shared<normal_msg>();
        probe->put_msg(handler_id, msg);
    }

    // sanity：现在 handler 应该一条都没收到。
    if (handler->handle_msg_calls.load() != 0) {
        std::cerr << "FAIL: messages somehow delivered before watchdog fired (test setup off)\n";
        return false;
    }

    // 等过 _time_length，让 watchdog 判定 elapsed > _time_length AND len > 0。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // 触发 watchdog timer：进入 channel_data_process::handle_timeout，
    // 调 set_real_net(true) 把 channel 推进 _obj_net_map。
    std::vector<uint32_t> exp_vec;
    container.get_timer()->check_timer(exp_vec);

    if (probe->handle_timeout_calls.load() == 0) {
        std::cerr << "FAIL: watchdog timer never reached channel_data_process::handle_timeout\n";
        return false;
    }

    // 这一步是验证 obj_process 的修复：channel 的 connect wants_tick() 永远是 false
    // (无 EPOLLOUT、无 codec)。修复前会被过滤掉 real_net_process 不会跑；修复后
    // _obj_net_map 成员无脑跑一次 real_net_process → real_recv → process_recv_buf
    // → drain queue → 走 container.handle_msg 把消息派到 handler。
    container.obj_process();

    if (handler->handle_msg_calls.load() != kStuck) {
        std::cerr << "FAIL: expected " << kStuck << " messages delivered after recovery, got "
                  << handler->handle_msg_calls.load() << ". Either watchdog didn't push "
                     "channel into _obj_net_map (channel_data_process.cpp fix), or obj_process "
                     "still gates real_net_process behind wants_tick (common_obj_container.cpp fix).\n";
        return false;
    }

    std::cout << "PASS: " << kStuck << " stuck messages drained after watchdog recovery\n";
    return true;
}

} // namespace

int main() {
    int failures = 0;
    if (!test1_timer_routing())          ++failures;
    if (!test2_stuck_channel_recovery()) ++failures;

    if (failures == 0) {
        std::cout << "\n=== ALL TESTS PASSED ===\n";
        return 0;
    }
    std::cout << "\n=== " << failures << " TEST(S) FAILED ===\n";
    return 1;
}
