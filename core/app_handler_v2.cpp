#include "app_handler_v2.h"

#include "base_data_process.h"
#include "base_net_obj.h"
#include "base_net_thread.h"
#include "common_def.h"

namespace myframe {
namespace detail {

namespace {
thread_local ::base_data_process* g_current_process = nullptr;
}

HandlerContextScope::HandlerContextScope(::base_data_process* process)
    : _previous(g_current_process)
{
    g_current_process = process;
}

HandlerContextScope::~HandlerContextScope()
{
    g_current_process = _previous;
}

::base_data_process* current_process()
{
    return g_current_process;
}

} // namespace detail

uint32_t IApplicationHandler::schedule_timeout(uint32_t delay_ms, uint32_t timer_type) const
{
    if (delay_ms == 0) {
        delay_ms = 1;
    }

    ::base_data_process* process = detail::current_process();
    if (!process) {
        return 0;
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return 0;
    }

    std::shared_ptr<timer_msg> timer(new timer_msg);
    timer->_obj_id = net->get_id()._id;
    timer->_timer_type = timer_type ? timer_type : APPLICATION_TIMER_TYPE;
    timer->_time_length = delay_ms;

    process->add_timer(timer);
    return timer->_timer_id;
}

::base_net_thread* IApplicationHandler::get_current_thread() const
{
    ::base_data_process* process = detail::current_process();
    if (!process) {
        return nullptr;
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return nullptr;
    }

    const ObjId& id = net->get_id();
    return base_net_thread::get_base_net_thread_obj(id._thread_index);
}

ObjId IApplicationHandler::current_connection_id() const
{
    ::base_data_process* process = detail::current_process();
    if (!process) {
        return ObjId{};
    }

    std::shared_ptr<base_net_obj> net = process->get_base_net();
    if (!net) {
        return ObjId{};
    }
    return net->get_id();
}

} // namespace myframe
