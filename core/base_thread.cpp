#include "base_thread.h"
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif


base_thread::base_thread():_thread_id(0), _run_flag(true)
{
    std::lock_guard<std::mutex> lck(base_thread::_mutex);
    _thread_index_start++;
    _thread_index = _thread_index_start;
}

base_thread::~base_thread()
{
}

bool base_thread::start()
{
    if (_thread_id) {
        return false;
    }

    int ret = pthread_create(&_thread_id, NULL, base_thread_proc, this);
    if (ret != 0)
    {
        return false;
    }
    _thread_vec.push_back(this);
    return true;
}

void base_thread::join_thread()
{
    pthread_join(_thread_id, NULL);
}

bool base_thread::stop() 
{
    _run_flag = false;
    return true;
}


void * base_thread::base_thread_proc(void *arg)
{
    base_thread *p = (base_thread*)arg;
#ifdef __linux__
    // Optional CPU affinity: MYFRAME_THREAD_AFFINITY=1 enables binding
    if (const char* env = ::getenv("MYFRAME_THREAD_AFFINITY")) {
        if (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0) {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            if (ncpu > 0) {
                int core = (int)((p->get_thread_index() - 1) % ncpu);
                cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
                pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
            }
        }
    }
#endif
    return p->run();
}

void base_thread::stop_all_thread()
{
    std::vector<base_thread*>::iterator it;
    for (it = _thread_vec.begin(); it != _thread_vec.end(); it++){
        (*it)->stop();
    }
}

void base_thread::join_all_thread()
{
    std::vector<base_thread*>::iterator it;
    for (it = _thread_vec.begin(); it != _thread_vec.end(); it++){
        (*it)->join_thread();
    }
}


bool base_thread::get_run_flag()
{
    return _run_flag;
}

void base_thread::set_run_flag(bool flag)
{
    _run_flag = flag;
}

pthread_t base_thread::get_thread_id()
{
    return _thread_id;
}

uint32_t base_thread::get_thread_index()
{
    return _thread_index;
}


std::vector<base_thread*> base_thread::_thread_vec;

// 明确初始化为 0，避免不同编译器/链接器差异引起的潜在未定义行为
uint32_t base_thread::_thread_index_start = 0;

std::mutex base_thread::_mutex;
