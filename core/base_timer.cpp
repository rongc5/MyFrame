
#include "base_timer.h"
#include "common_util.h"
#include "base_net_thread.h"
#include "common_obj_container.h"

base_timer::base_timer(common_obj_container * net_container)
{
    _net_container = net_container;
    _timerid = TIMER_ID_BEGIN;
}

base_timer::~base_timer()
{
    _timer_list.clear();
}

uint32_t base_timer::gen_timerid()
{
    do
    {
        _timerid++;
        if (_timerid < TIMER_ID_BEGIN)
            _timerid = TIMER_ID_BEGIN;
        
    }while (_timerid_set.count(_timerid));

    _timerid_set.insert(_timerid);

    return _timerid;
}

uint32_t base_timer::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    if (t_msg->_time_length <= 0 || t_msg->_obj_id <= 0)
    {
        PDEBUG("add_timer failed: time_length:%u timer_id:%u _timer_type:%u", 
            t_msg->_time_length, t_msg->_timer_id, t_msg->_timer_type);
        return 0;
    }

    uint64_t reach_time = GetMonotonicMilliSecond() + t_msg->_time_length;
    t_msg->_timer_id = gen_timerid();

    //PDEBUG("time_length:%u reach_time:%llu timer_id:%u _timer_type:%u", t_msg->_time_length, reach_time, t_msg->_timer_id, t_msg->_timer_type);

    _timer_list.insert(make_pair(reach_time, t_msg));

    return t_msg->_timer_id;
}

void base_timer::check_timer(std::vector<uint32_t> &expect_list)
{
    const uint64_t now = GetMonotonicMilliSecond();

    // Detach every expired entry before running callbacks, so a callback can
    // safely add timers (including re-arming the same timer_msg).
    std::vector<std::shared_ptr<timer_msg> > expired;
    auto end = _timer_list.upper_bound(now);
    for (auto it = _timer_list.begin(); it != end; ++it)
    {
        _timerid_set.erase(it->second->_timer_id);
        expired.push_back(it->second);
    }
    _timer_list.erase(_timer_list.begin(), end);

    for (auto & t_msg : expired)
    {
        const uint32_t obj_id = t_msg->_obj_id;
        try
        {
            _net_container->handle_timeout(t_msg);
        }
        catch(CMyCommonException &e)
        {
            expect_list.push_back(obj_id);
        }
        catch(std::exception &e)
        {
            expect_list.push_back(obj_id);
        }
    }
}

bool base_timer::is_empty()
{
    return _timer_list.empty();
}
