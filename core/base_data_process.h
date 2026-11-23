#ifndef _BASE_DATA_PROCESS_H_
#define _BASE_DATA_PROCESS_H_

#include "common_util.h"
#include "outbound_guard.h"
#include <deque>
#include <functional>
#include <memory>

class base_net_obj;
class base_data_process
{
    public:
        base_data_process(std::shared_ptr<base_net_obj> p);

        virtual ~base_data_process();

        virtual void peer_close();

        virtual std::string *get_send_buf();

        virtual void reset();

        virtual size_t process_recv_buf(const char *buf, size_t len);

        // 每个事件循环 tick 调用一次 (obj_process 遍历时), 与是否有可读数据无关。
        // 默认空操作; channel_data_process 用它做"无条件排空队列", 保证即使唤醒字节
        // 丢失也能在一个 epoll 超时内处理掉积压消息 (参考 rxlogger 的 obj_process 模型)。
        virtual void on_tick() {}

        virtual void handle_msg(std::shared_ptr<normal_msg> & p_msg);

        void add_timer(std::shared_ptr<timer_msg> & t_msg);

        virtual void handle_timeout(std::shared_ptr<timer_msg> & t_msg);

        // Ensure peer_close() callback fires exactly once across all teardown paths.
        void notify_peer_close();

        void put_send_buf(std::string * str);
        // Helpers that leverage per-thread string pool
        void put_send_copy(const std::string& data);
        void put_send_move(std::string&& data);

        std::shared_ptr<base_net_obj>  get_base_net();

        virtual void destroy();

        // Request to close the underlying connection gracefully.
        // These helpers schedule a DELAY_CLOSE timer which will be handled by
        // base_connect::handle_timeout and trigger connection teardown via the
        // container (ensuring epoll and maps are cleaned consistently).
        void request_close_now();
        void request_close_after(uint32_t delay_ms);

        // Immediate close: throw an exception up to the event loop so the
        // container executes the standard teardown path (destroy + erase),
        // which removes the fd from epoll and closes it in base_net_obj dtor.
        // Call this from within event-driven contexts (e.g., process_recv_buf,
        // handle_msg, msg_recv_finish) to immediately drop the connection.
        [[noreturn]] void close_now();

        // Whether this process currently wants to receive data from socket.
        // Default true; protocols can override to gate EPOLLIN handling
        // (e.g., HTTP client while still sending request).
        virtual bool want_recv() const { return true; }

        // Whether this process prefers using MSG_PEEK to avoid consuming data
        // during early protocol detection (e.g. keep TLS ClientHello available).
        // Default false; detectors override to request peeking.
        virtual bool want_peek() const { return false; }

        // Virtual function for getting process name (for debugging/logging)
        virtual const char* name() const { return "base_data_process"; }

        // ===== Owner-thread helpers =====
        // True when the caller may touch this process' send state directly
        // (see base_net_obj::on_owner_thread()).
        bool on_owner_thread() const;
        // Post fn to the connection owner thread. fn runs only if this process
        // object is still alive (not replaced/destroyed) when it is executed.
        bool post_to_owner_thread(std::function<void()> fn);
        // Run fn inline on the owner thread, otherwise post it.
        bool run_on_owner_thread(std::function<void()> fn);

        // ===== Outbound backpressure =====
        // Per-connection high-water mark in bytes; 0 = global default
        // (myframe::outbound_high_water_mark()).
        void set_outbound_high_water_mark(size_t bytes);
        size_t outbound_high_water_mark() const;
        // Bytes accepted for sending that the socket layer has not pulled yet.
        size_t outbound_queued_bytes() const;
        bool is_slow_consumer() const;
        // Thread-safe handle to the counters (never to the process itself).
        std::weak_ptr<myframe::detail::OutboundState> outbound_state() const { return _outbound; }

    protected:
        void clear_send_list();

        // Latch the slow-consumer state, count it, and ask the owner thread to
        // close the connection. Safe from any thread.
        void trigger_slow_consumer_close(myframe::OutboundKind kind);

    protected:
        std::weak_ptr<base_net_obj> _p_connect;
        std::shared_ptr<myframe::detail::OutboundState> _outbound;
        std::deque<std::string*> _send_list;
        bool _closing{false};
        bool _peer_close_notified{false};
};

#endif
