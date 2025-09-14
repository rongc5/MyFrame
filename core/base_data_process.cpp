#include "base_net_obj.h"
#include "base_data_process.h"
#include "common_exception.h"



base_data_process::base_data_process(std::shared_ptr<base_net_obj> p)
{
    _p_connect = p;
    PDEBUG("%p", this);
}

base_data_process::~base_data_process()
{
    PDEBUG("%p", this);
    clear_send_list();
}

void base_data_process::peer_close()
{
    PDEBUG("%p", this);
}

std::string * base_data_process::get_send_buf()
{
    PDEBUG("%p", this);
    if (_send_list.begin() == _send_list.end()) {
        PDEBUG("_send_list is empty");
        return NULL;
    }

    std::string *p = *(_send_list.begin());
    _send_list.erase(_send_list.begin());

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
    for (std::list<std::string*>::iterator itr = _send_list.begin(); itr != _send_list.end(); ++itr)
    {
        delete *itr;
    }

    _send_list.clear();
}

void base_data_process::put_send_buf(std::string * str)
{
    _send_list.push_back(str);
    if (auto sp = _p_connect.lock())
    {
        sp->notice_send();  
    }
}

std::shared_ptr<base_net_obj>  base_data_process::get_base_net()
{
    return _p_connect.lock();
}

void base_data_process::destroy()
{
    PDEBUG("%p", this);
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
    // Mirror the peer-close exception path to trigger container teardown:
    // - common_obj_container catches the exception
    // - calls destroy() on the net object (process-level cleanup)
    // - erases the object from maps; base_net_obj dtor removes epoll and closes fd
    THROW_COMMON_EXCEPT("process requested close");
}
