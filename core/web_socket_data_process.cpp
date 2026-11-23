#include "web_socket_data_process.h"
#include "web_socket_msg.h"
#include "web_socket_process.h"

#include "base_net_obj.h"
#include "string_pool.h"
#include "common_exception.h"
#include "obj_task_msg.h"



web_socket_data_process::web_socket_data_process(web_socket_process *p):base_data_process(p->get_base_net())
{
    _process = p;
    PDEBUG("%p", this);
}

web_socket_data_process::~web_socket_data_process()
{
    PDEBUG("%p", this);
    std::lock_guard<std::mutex> lock(_send_list_mutex);
    for (auto& msg : _send_list) {
        myframe::string_release(msg._p_msg);
    }
    _send_list.clear();
}

void web_socket_data_process::on_handshake_ok()		
{
    PDEBUG("%p", this);
}

void web_socket_data_process::on_ping(const char op_code, const std::string &ping_data)
{
    PDEBUG("%p", this);
    if (op_code == 0x09 && _process)
    {
        _process->send_ping(0x0a, ping_data);
    }
}

void web_socket_data_process::peer_close()
{
    on_close();  // 调用子类可重写的回调
    base_data_process::peer_close();  // 调用基类实现
}

uint64_t web_socket_data_process::get_timeout_len()
{
    return WS_CONNECT_TIMEOUT;
}

std::string *web_socket_data_process::get_send_buf()
{	
    std::string *p_ret = NULL;
    ws_msg_type tmp = get_send_msg();
    p_ret = tmp._p_msg;
    return p_ret;
}

uint64_t web_socket_data_process::get_next_send_len(int8_t &content_type)
{
    std::lock_guard<std::mutex> lock(_send_list_mutex);
    uint64_t len = 0;
    if (_send_list.begin() != _send_list.end())
    {
        len = _send_list.begin()->_p_msg->length();
        content_type = _send_list.begin()->_con_type;
    }
    return len;
}

void web_socket_data_process::put_send_msg(ws_msg_type msg)
{
    const size_t n = msg._p_msg ? msg._p_msg->size() : 0;
    if (!_outbound->try_reserve(n)) {
        // Slow consumer: drop the frame and close the connection so queued
        // memory stays bounded.
        myframe::detail::note_dropped(myframe::OutboundKind::WebSocket, n);
        myframe::string_release(msg._p_msg);
        trigger_slow_consumer_close(myframe::OutboundKind::WebSocket);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_send_list_mutex);
        _send_list.push_back(msg);
    }
    auto sp = _p_connect.lock();
    if (!sp || sp->on_owner_thread()) {
        // Only sends once the handshake has completed (checked by notice_send).
        _process->notice_send();
        return;
    }
    request_owner_flush();
}

void web_socket_data_process::request_owner_flush()
{
    if (_outbound->flush_posted.exchange(true, std::memory_order_acq_rel)) {
        return;  // a flush is already queued on the owner thread
    }
    bool posted = post_to_owner_thread([this]() {
        _outbound->flush_posted.store(false, std::memory_order_release);
        _process->notice_send();
    });
    if (!posted) {
        _outbound->flush_posted.store(false, std::memory_order_release);
    }
}

void web_socket_data_process::enqueue_reserved(ws_msg_type msg, size_t reserved)
{
    if (_closing || _outbound->slow_consumer.load(std::memory_order_acquire)) {
        _outbound->release(reserved);
        myframe::string_release(msg._p_msg);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_send_list_mutex);
        _send_list.push_back(msg);
    }
    _process->notice_send();
}

ws_send_handle web_socket_data_process::send_handle()
{
    ws_send_handle h;
    if (auto sp = _p_connect.lock()) {
        h.id = sp->get_id();
        h.generation = sp->generation();
        h.proc = this;
        h.state = _outbound;
    }
    return h;
}

bool web_socket_data_process::post_send(const ws_send_handle & h,
                                        std::shared_ptr<const std::string> payload,
                                        int8_t con_type)
{
    if (!h.valid() || !payload) return false;
    // Foreign-thread access is limited to the shared counters.
    std::shared_ptr<myframe::detail::OutboundState> st = h.state.lock();
    if (!st) return false;  // connection already gone

    const size_t n = payload->size();
    if (!st->try_reserve(n)) {
        myframe::detail::note_dropped(myframe::OutboundKind::WebSocket, n);
        if (st->mark_slow_consumer()) {
            myframe::detail::note_slow_consumer_closed(myframe::OutboundKind::WebSocket);
            std::weak_ptr<myframe::detail::OutboundState> alive = h.state;
            post_obj_task(h.id, h.generation, [alive](base_net_obj &) {
                if (!myframe::detail::process_alive(alive)) return;
                THROW_COMMON_EXCEPT("slow consumer: outbound high-water mark exceeded");
            });
        }
        return false;
    }

    web_socket_data_process * proc = h.proc;
    std::weak_ptr<myframe::detail::OutboundState> alive = h.state;
    bool posted = post_obj_task(h.id, h.generation,
        [proc, alive, payload, con_type, n](base_net_obj &) {
            // Generation already matched; the liveness flag proves proc is
            // the same, still-attached process object.
            if (!myframe::detail::process_alive(alive)) return;
            ws_msg_type m;
            m.init();
            m._p_msg = myframe::string_acquire();
            m._p_msg->assign(*payload);
            m._con_type = con_type;
            proc->enqueue_reserved(m, n);
        });
    if (!posted) {
        st->release(n);
    }
    return posted;
}

ws_msg_type web_socket_data_process::get_send_msg()
{
    std::lock_guard<std::mutex> lock(_send_list_mutex);
    ws_msg_type ret;
    if (_send_list.begin() != _send_list.end())
    {
        ret = _send_list.front();
        _send_list.pop_front();
        if (ret._p_msg) _outbound->release(ret._p_msg->size());
    }
    return ret;
}

size_t web_socket_data_process::process_recv_buf(const char *buf, size_t len)
{
    _recent_msg.append(buf, len); 
    return len;
}
