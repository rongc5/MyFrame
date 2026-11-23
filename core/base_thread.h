#ifndef __BASE_THREAD_H__
#define __BASE_THREAD_H__

#include "common_def.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

class base_thread
{
    public:
        base_thread();


        virtual ~base_thread();


        virtual bool start();


        void join_thread();


        virtual bool stop();


        virtual void* run() = 0;


        static void *base_thread_proc(void *arg);


        static void stop_all_thread();


        static void join_all_thread();


        bool get_run_flag();


        void set_run_flag(bool flag);


        pthread_t get_thread_id();

        uint32_t get_thread_index();

    protected:
        uint32_t _thread_index;

        pthread_t _thread_id;
        std::atomic<bool> _run_flag;

        // Guards _thread_id, _started and _joining. Never held across pthread_join.
        std::mutex _lifecycle_mutex;
        std::condition_variable _join_cv;
        bool _started{false};  // Keeps start() single-shot after _thread_id is cleared.
        bool _joining{false};

        static std::vector<base_thread*>	_thread_vec;

        static uint32_t _thread_index_start;
        static std::mutex _mutex;
};


#endif

