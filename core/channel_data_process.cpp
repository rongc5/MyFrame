#include "channel_data_process.h"
#include "base_net_thread.h"
#include "base_net_obj.h"
#include "base_connect.h"

#include "base_net_obj.h"
#include "common_util.h"

channel_data_process::channel_data_process(std::shared_ptr<base_net_obj> p, int channelid):base_data_process(p),
    _channelid(channelid),
    _current(0)
{
    _last_time = 0;
}

size_t channel_data_process::process_recv_buf(const char *buf, size_t len)
{
    std::deque<normal_obj_msg > processing_queue;
    size_t k = len / sizeof(CHANNEL_MSG_TAG); //防止在handle_msg 中调用put_msg d导致死锁
    {
        std::lock_guard<std::mutex> lck (_mutex);
        if (_queue[_current].empty())
        {
            _current = 1 - _current;
        }
        processing_queue.swap(_queue[_current]);
    }
    PDEBUG("buf:%s, len:%zu, que.len:%zu", buf, len, processing_queue.size());

    size_t i = 0;
    auto sp = _p_connect.lock();
    while (!processing_queue.empty())
    {
        if (sp)
        {
            normal_obj_msg &msg = processing_queue.front();
            sp->get_net_container()->handle_msg(msg._id, msg.p_msg);
        }
        processing_queue.pop_front();
        ++i;
    }

     k =  i * sizeof(CHANNEL_MSG_TAG);

     if (!_last_time)
         add_event_timer();

     _last_time = GetMilliSecond();

    return k;
}


void channel_data_process::put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    std::lock_guard<std::mutex> lck (_mutex);
    normal_obj_msg nbj_msg;
    nbj_msg.p_msg = p_msg;
    nbj_msg._id = obj_id;
    int idle = 1 - _current;
    _queue[idle].push_back(nbj_msg);

    //write(_channelid, CHANNEL_MSG_TAG, sizeof(CHANNEL_MSG_TAG));
    send(_channelid, CHANNEL_MSG_TAG, sizeof(CHANNEL_MSG_TAG), MSG_DONTWAIT);

    return ;
}

void channel_data_process::add_event_timer(uint64_t time_out)
{
    time_out = time_out > 100 * MAX_CHANNEL_EVENT_TIMEOUT ? 100 * MAX_CHANNEL_EVENT_TIMEOUT:time_out;
    time_out = time_out < 20 ? 20:time_out;

    std::shared_ptr<timer_msg> t_msg(new timer_msg);
    t_msg->_obj_id = OBJ_ID_DOMAIN;
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
