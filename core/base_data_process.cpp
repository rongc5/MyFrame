#include "base_net_obj.h"
#include "base_data_process.h"
#include "common_exception.h"
#include "string_pool.h"



namespace {
// Owns a pooled send buffer while it travels to the owner thread inside a
// (copyable) std::function; frees it if the task is dropped unrun.
struct pending_send_buf {
    explicit pending_send_buf(std::string* s) : p(s) {}
    ~pending_send_buf() { if (p) myframe::string_release(p); }
    std::string* release() { std::string* r = p; p = nullptr; return r; }
    std::string* p;
};
}

base_data_process::base_data_process(std::shared_ptr<base_net_obj> p)
    : _outbound(std::make_shared<myframe::detail::OutboundState>())
{
    _p_connect = p;
    PDEBUG("%p", this);
}

base_data_process::~base_data_process()
{
    PDEBUG("%p", this);
    _outbound->owner_alive.store(false, std::memory_order_release);
    clear_send_list();
}

void base_data_process::peer_close()
{
    PDEBUG("%p", this);
}

std::string * base_data_process::get_send_buf()
{
    PDEBUG("%p", this);
    if (_send_list.empty()) {
        PDEBUG("_send_list is empty");
        return NULL;
    }

    std::string *p = _send_list.front();
    _send_list.pop_front();
    if (p) _outbound->release(p->size());

    return p;
}

void base_data_process::reset()
{
    clear_send_list();
    PDEBUG("%p", this);
}

size_t base_data_process::process_recv_buf(const char *buf, size_t len)
{
    PDEBUG("%p", this);

    return len;
}

void base_data_process::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
    PDEBUG("%p", this);
}

void base_data_process::clear_send_list()
{
    for (auto* ptr : _send_list) {
        if (ptr) _outbound->release(ptr->size());
        myframe::string_release(ptr);
    }
    _send_list.clear();
}

void base_data_process::put_send_buf(std::string * str)
{
    if (!str) return;
    auto sp = _p_connect.lock();
    const size_t n = str->size();

    if (sp && !sp->on_owner_thread()) {
        // Foreign thread: never touch _send_list or the socket here. Reserve
        // the bytes (so in-flight data counts against the HWM) and hand the
        // buffer to the owner thread.
        if (!_outbound->try_reserve(n)) {
            myframe::detail::note_dropped(myframe::OutboundKind::Stream, n);
            myframe::string_release(str);
            trigger_slow_consumer_close(myframe::OutboundKind::Stream);
            return;
        }
        std::shared_ptr<pending_send_buf> holder = std::make_shared<pending_send_buf>(str);
        std::weak_ptr<myframe::detail::OutboundState> alive = _outbound;
        bool posted = sp->post_to_owner([this, alive, holder, n](base_net_obj & conn) {
            std::shared_ptr<myframe::detail::OutboundState> st = alive.lock();
            // Process replaced or destroyed: holder frees the buffer.
            if (!st || !st->owner_alive.load(std::memory_order_acquire)) return;
            std::string* buf = holder->release();
            if (_closing || st->slow_consumer.load(std::memory_order_acquire)) {
                st->release(n);
                myframe::string_release(buf);
                return;
            }
            _send_list.push_back(buf);
            conn.notice_send();
        });
        if (!posted) {
            _outbound->release(n);
        }
        return;
    }

    if (_closing) { myframe::string_release(str); return; }
    if (!_outbound->try_reserve(n)) {
        myframe::detail::note_dropped(myframe::OutboundKind::Stream, n);
        myframe::string_release(str);
        trigger_slow_consumer_close(myframe::OutboundKind::Stream);
        return;
    }
    _send_list.push_back(str);
    if (sp)
    {
        sp->notice_send();
    }
}

void base_data_process::put_send_copy(const std::string& data)
{
    std::string* s = myframe::string_acquire();
    s->assign(data);
    put_send_buf(s);
}

void base_data_process::put_send_move(std::string&& data)
{
    std::string* s = myframe::string_acquire();
    s->swap(data);
    put_send_buf(s);
}

std::shared_ptr<base_net_obj>  base_data_process::get_base_net()
{
    return _p_connect.lock();
}

void base_data_process::destroy()
{
    PDEBUG("%p", this);
    notify_peer_close();
}

void base_data_process::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    auto sp = _p_connect.lock();
    if (sp)
        sp->add_timer(t_msg);
}

void base_data_process::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    PDEBUG("%p", this);
}

void base_data_process::request_close_after(uint32_t delay_ms)
{
    auto sp = _p_connect.lock();
    if (!sp) return;
    if (_closing) return;
    _closing = true;
    std::shared_ptr<timer_msg> t_msg(new timer_msg);
    t_msg->_obj_id = sp->get_id()._id;
    t_msg->_timer_type = DELAY_CLOSE_TIMER_TYPE;
    t_msg->_time_length = delay_ms ? delay_ms : 1; // at least 1ms
    add_timer(t_msg);
}

void base_data_process::request_close_now()
{
    request_close_after(1);
}

[[noreturn]] void base_data_process::close_now()
{
    if (!_closing) _closing = true;
    // Mirror the peer-close exception path to trigger container teardown:
    // - common_obj_container catches the exception
    // - calls destroy() on the net object (process-level cleanup)
    // - erases the object from maps; base_net_obj dtor removes epoll and closes fd
    THROW_COMMON_EXCEPT("process requested close");
}

void base_data_process::notify_peer_close()
{
    if (_peer_close_notified) {
        _closing = true;
        return;
    }
    _peer_close_notified = true;
    _closing = true;
    peer_close();
}

bool base_data_process::on_owner_thread() const
{
    auto sp = _p_connect.lock();
    return !sp || sp->on_owner_thread();
}

bool base_data_process::post_to_owner_thread(std::function<void()> fn)
{
    if (!fn) return false;
    auto sp = _p_connect.lock();
    if (!sp) return false;
    std::weak_ptr<myframe::detail::OutboundState> alive = _outbound;
    return sp->post_to_owner([alive, fn](base_net_obj &) {
        if (myframe::detail::process_alive(alive)) fn();
    });
}

bool base_data_process::run_on_owner_thread(std::function<void()> fn)
{
    if (!fn) return false;
    if (on_owner_thread()) {
        fn();
        return true;
    }
    return post_to_owner_thread(std::move(fn));
}

void base_data_process::set_outbound_high_water_mark(size_t bytes)
{
    _outbound->hwm_override.store(bytes, std::memory_order_relaxed);
}

size_t base_data_process::outbound_high_water_mark() const
{
    return _outbound->limit();
}

size_t base_data_process::outbound_queued_bytes() const
{
    return _outbound->queued_bytes.load(std::memory_order_relaxed);
}

bool base_data_process::is_slow_consumer() const
{
    return _outbound->slow_consumer.load(std::memory_order_acquire);
}

void base_data_process::trigger_slow_consumer_close(myframe::OutboundKind kind)
{
    if (!_outbound->mark_slow_consumer()) {
        return;  // already closing
    }
    myframe::detail::note_slow_consumer_closed(kind);
    if (auto sp = _p_connect.lock()) {
        sp->post_close_to_owner("slow consumer: outbound high-water mark exceeded");
    }
}
