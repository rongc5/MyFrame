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
    std::lock_guard<std::mutex> lck(_mutex);
    for (auto it = _thread_vec.begin(); it != _thread_vec.end(); ++it) {
        if (*it == this) {
            _thread_vec.erase(it);
            break;
        }
    }
}

bool base_thread::start()
{
    {
        // Held across pthread_create so the new thread observes a published id.
        std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);
        if (_started) {
            return false;
        }

        pthread_t tid;
        int ret = pthread_create(&tid, NULL, base_thread_proc, this);
        if (ret != 0)
        {
            return false;
        }
        _thread_id = tid;
        _started = true;
    }
    {
        std::lock_guard<std::mutex> lck(_mutex);
        _thread_vec.push_back(this);
    }
    return true;
}

void base_thread::join_thread()
{
    std::unique_lock<std::mutex> lifecycle(_lifecycle_mutex);
    if (!_thread_id || pthread_equal(_thread_id, pthread_self())) {
        // Never started, already joined, or a self-join that would deadlock.
        return;
    }
    if (_joining) {
        // Another caller owns the join; return only once the thread has exited.
        _join_cv.wait(lifecycle, [this] { return !_joining; });
        return;
    }

    _joining = true;
    pthread_t tid = _thread_id;
    lifecycle.unlock();
    int ret = pthread_join(tid, NULL);
    lifecycle.lock();
    if (ret == 0) {
        _thread_id = 0;
    }
    _joining = false;
    _join_cv.notify_all();
}

bool base_thread::stop() 
{
    _run_flag.store(false);
    return true;
}


void * base_thread::base_thread_proc(void *arg)
{
    base_thread *p = (base_thread*)arg;
#ifdef __linux__
    // Optional CPU affinity: enabled if MYFRAME_THREAD_AFFINITY=1 or MYFRAME_PERF_PRESET=1
    auto want_on = [](){
        const char* env = ::getenv("MYFRAME_THREAD_AFFINITY");
        if (env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0)) return true;
        const char* preset = ::getenv("MYFRAME_PERF_PRESET");
        if (preset && (strcmp(preset, "0") != 0 && strcasecmp(preset, "false") != 0)) return true;
        return false;
    }();
    if (want_on) {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            if (ncpu > 0) {
                int core = (int)((p->get_thread_index() - 1) % ncpu);
                cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
                pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
            }
    }
#endif
    return p->run();
}

void base_thread::stop_all_thread()
{
    std::vector<base_thread*> snapshot;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        snapshot = _thread_vec;
    }
    for (auto* t : snapshot) {
        t->stop();
    }
}

void base_thread::join_all_thread()
{
    std::vector<base_thread*> snapshot;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        snapshot = _thread_vec;
    }
    for (auto* t : snapshot) {
        t->join_thread();
    }
}


bool base_thread::get_run_flag()
{
    return _run_flag.load();
}

void base_thread::set_run_flag(bool flag)
{
    _run_flag.store(flag);
}

pthread_t base_thread::get_thread_id()
{
    std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);
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
