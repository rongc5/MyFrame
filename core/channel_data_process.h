#ifndef _CHANNEL_DATA_PROCESS_H_
#define _CHANNEL_DATA_PROCESS_H_

#include "common_def.h"
#include "base_data_process.h"

class normal_obj_msg //内部传递的消息
{
    public:
        uint32_t _id;
        std::shared_ptr<normal_msg>  p_msg;

        virtual ~normal_obj_msg(){
        }   
};


class base_net_obj;
class channel_data_process:public base_data_process
{
    public:
        channel_data_process(std::shared_ptr<base_net_obj> p, int channelid);

        virtual ~channel_data_process()
        {
            for (auto &que : _queue)
            {
                que.clear();
            }
        }	

        virtual size_t process_recv_buf(const char *buf, size_t len);

        // 每 tick 无条件排空队列 (rxlogger 模型): 即使唤醒字节丢失也保证消息被处理。
        virtual void on_tick() override;

        void put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg);

        void add_event_timer(uint64_t time_out = MAX_CHANNEL_EVENT_TIMEOUT);

        virtual void handle_timeout(std::shared_ptr<timer_msg> & t_msg);

    protected:
        // 排空队列并处理 (swap 整个队列 + 逐条 handle_msg)。
        // 由 process_recv_buf(唤醒字节路径) 与 on_tick(周期兜底路径) 共用。
        void drain_queue();

        std::mutex _mutex;
        std::deque<normal_obj_msg > _queue[2];
        int _current;
        int _channelid;
        uint64_t _last_time;
        // 合并唤醒(coalesced): 仅在队列 空->非空 边沿往 socketpair 写一个 tag 字节,
        // 管道里恒 <=1 字节, 不会因每消息写字节而堆满缓冲/EAGAIN 风暴。
        // 唤醒字节只是"降低延迟"的优化; 正确性由 on_tick 的周期无条件排空保证,
        // 故即便这个字节因 EPOLLET 边沿竞争丢失, 也最多延迟一个 epoll 超时被处理。
        bool _wake_armed;
};

#endif
