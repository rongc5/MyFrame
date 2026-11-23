#include "channel_data_process.h"
#include "base_net_thread.h"
#include "base_net_obj.h"
#include "base_connect.h"

#include "base_net_obj.h"
#include "common_util.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

channel_data_process::channel_data_process(std::shared_ptr<base_net_obj> p, int channelid):base_data_process(p),
    _channelid(channelid),
    _current(0)
{
    _last_time = 0;
    _wake_armed = false;
}

// 单次最多处理多少条, 避免慢 handler 长时间占住 net 线程, 推迟 IO/timer/plugin tick。
// 剩余消息保持入队, 由下一轮 tick 继续 (并写一个唤醒字节让 epoll 立刻返回, 不必等满超时)。
static const size_t kMaxBatchPerTick = 1024;

// 排空队列(受 batch 预算限制): 取出至多 kMaxBatchPerTick 条, 锁外逐条 handle_msg。
// 唤醒字节只是信号; 这里按预算处理, 即使多写/丢字节也不影响正确性 (run() 每轮都会再调)。
void channel_data_process::drain_queue()
{
    std::deque<normal_obj_msg > batch;
    bool more = false;
    {
        std::lock_guard<std::mutex> lck (_mutex);
        if (_queue[_current].empty())
        {
            _current = 1 - _current;
        }
        std::deque<normal_obj_msg > &q = _queue[_current];
        if (q.empty())
        {
            _wake_armed = false;
            return;
        }
        for (size_t i = 0; i < kMaxBatchPerTick && !q.empty(); ++i)
        {
            batch.push_back(std::move(q.front()));
            q.pop_front();
        }
        // Check both queues: a producer may have coalesced onto the current wake
        // while pushing to the idle queue, and that message still needs a wake.
        more = !_queue[0].empty() || !_queue[1].empty();
        // 还有剩余 -> 保持武装(待处理); 已排空 -> 解除武装, 之后 producer push 会重新写字节。
        _wake_armed = more;
    }

    auto sp = _p_connect.lock();
    while (!batch.empty())
    {
        if (sp)
        {
            normal_obj_msg &msg = batch.front();
            sp->get_net_container()->handle_msg(msg._id, msg.p_msg);
        }
        batch.pop_front();
    }

    // 本轮没排完: 写一个唤醒字节, 让下一轮 obj_process 的 epoll_wait 立刻返回继续排,
    // 同时本 tick 已让出 CPU 给 IO/timer/plugin (不会被一个超大 batch 长时间霸占)。
    if (more)
    {
        static const char kWakeByte = 'c';
        ::send(_channelid, &kWakeByte, 1, MSG_DONTWAIT);
    }
}

size_t channel_data_process::process_recv_buf(const char *buf, size_t len)
{
    PDEBUG("buf:%s, len:%zu", buf, len);
    drain_queue();

    // 第二道兜底: watchdog 定时器。on_tick 是主路径(每轮无条件 drain), 但如果某 channel
    // 异常脱离 net 线程的 run() 循环(例如被踢出 epoll 又不在 _channel_msg_vec 处理路径),
    // 这个定时器(handle_timeout 检测 queue 积压 + 超时)会 set_real_net(true) 把它推回
    // _obj_net_map 由 obj_process 再处理一次。首次收到唤醒时装上, 自我续期。
    if (!_last_time)
    {
        add_event_timer();
    }
    _last_time = GetMilliSecond();

    // 唤醒字节是纯信号; 已排空队列, 把收到的 wake 字节全部消费掉。
    // (调用方按返回值 erase recv_buf, 见 base_connect::real_recv)
    return len;
}

// 每个事件循环 tick 调用 (与是否有可读数据无关)。
// 这是正确性保证: 即使唤醒字节因 EPOLLET 边沿竞争/缓冲满而丢失, 积压的消息也会在
// 一个 epoll 超时(<=50ms)内被排空 —— 唤醒字节退化为纯延迟优化, 不再是正确性依赖。
// 参考 rxlogger::log_thread::obj_process: 每轮无条件 process_recv_buf。
void channel_data_process::on_tick()
{
    drain_queue();
}


void channel_data_process::put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    size_t queued_len = 0;
    bool need_wake = false;
    {
        std::lock_guard<std::mutex> lck (_mutex);
        normal_obj_msg nbj_msg;
        nbj_msg.p_msg = p_msg;
        nbj_msg._id = obj_id;
        int idle = 1 - _current;
        _queue[idle].push_back(nbj_msg);
        queued_len = _queue[0].size() + _queue[1].size();

        // Coalesced wakeup: only the producer that arms the flag writes a byte.
        need_wake = !_wake_armed;
        if (need_wake) {
            _wake_armed = true;
        }
    }

    if (!need_wake) {
        return;
    }

    // The syscall runs outside _mutex. If a drain wins the race the byte is only a
    // spurious wakeup. Write exactly one byte: CHANNEL_MSG_TAG has sizeof 2.
    static const char kWakeByte = 'c';
    ssize_t send_ret = ::send(_channelid, &kWakeByte, 1, MSG_DONTWAIT);
    if (send_ret == 1) {
        return;
    }

    int send_errno = errno;
    // A full buffer already holds an unread wake byte, so the wake is not lost.
    if (send_errno == EAGAIN || send_errno == EWOULDBLOCK) {
        return;
    }

    {
        // Let the next producer retry; on_tick remains the delivery backstop.
        std::lock_guard<std::mutex> lck (_mutex);
        if (!_queue[0].empty() || !_queue[1].empty()) {
            _wake_armed = false;
        }
    }

    static std::atomic<uint64_t> s_last_warn_ms{0};
    uint64_t now_ms = GetMilliSecond();
    uint64_t prev = s_last_warn_ms.load(std::memory_order_relaxed);
    if (now_ms - prev > 1000 &&
        s_last_warn_ms.compare_exchange_strong(prev, now_ms)) {
        fprintf(stderr,
                "[channel] tag send error: fd=%d ret=%zd errno=%d (%s) queued=%zu obj_id=%u\n",
                _channelid, send_ret, send_errno, strerror(send_errno),
                queued_len, obj_id);
    }
}

void channel_data_process::add_event_timer(uint64_t time_out)
{
    time_out = time_out > 100 * MAX_CHANNEL_EVENT_TIMEOUT ? 100 * MAX_CHANNEL_EVENT_TIMEOUT:time_out;
    time_out = time_out < 20 ? 20:time_out;

    std::shared_ptr<timer_msg> t_msg(new timer_msg);
    if (auto sp = _p_connect.lock()) {
        t_msg->_obj_id = sp->get_id()._id;
    } else {
        t_msg->_obj_id = OBJ_ID_DOMAIN;
    }
    t_msg->_timer_type = NONE_CHANNEL_EVENT_TIMER_TYPE;
    t_msg->_time_length = time_out;

    add_timer(t_msg);
}

void channel_data_process::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    if (t_msg == nullptr)
        return;

    int len = 0;
    {
        std::lock_guard<std::mutex> lck (_mutex);
        len = _queue[0].size() + _queue[1].size();
    }

    PDEBUG("handle_timeout: timer_id:%u timer_type:%u, len:%d", t_msg->_timer_id, t_msg->_timer_type, len);

    uint64_t now =  GetMilliSecond();
   if (t_msg->_timer_type == NONE_CHANNEL_EVENT_TIMER_TYPE)
   {
       if (llabs(now - _last_time) > t_msg->_time_length && len)
       {
           get_base_net()->set_real_net(true);
           add_event_timer(t_msg->_time_length/2);
       }
       else
       {
           add_event_timer(t_msg->_time_length * 2);
       }
   }
}
