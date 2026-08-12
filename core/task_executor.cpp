// task_executor.cpp · TaskExecutor 实现
#include "task_executor.h"
#include <cstdlib>

namespace myframe {

TaskExecutor::TaskExecutor(size_t threads, size_t max_queue)
    : _max_queue(max_queue), _stop(false) {
    if (threads == 0) threads = 1;
    for (size_t i = 0; i < threads; ++i)
        _workers.emplace_back(&TaskExecutor::worker_loop, this);
}

TaskExecutor::~TaskExecutor() { shutdown(); }

bool TaskExecutor::submit(std::function<void()> task) {
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_stop) return false;
        if (_q.size() >= _max_queue) return false;   // 队列满 → 拒绝
        _q.push_back(std::move(task));
    }
    _cv.notify_one();
    return true;
}

void TaskExecutor::shutdown() {
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_stop) return;
        _stop = true;
        _q.clear();   // 取消尚未开始的排队任务（不排空执行），保证及时停机
    }
    _cv.notify_all();
    // 等待正在执行的任务自然结束（无法强制中断线程；故要求任务本身有界、不长期阻塞）。
    for (std::thread& t : _workers) if (t.joinable()) t.join();
    _workers.clear();
}

size_t TaskExecutor::queue_size() const {
    std::lock_guard<std::mutex> lk(_mu);
    return _q.size();
}

void TaskExecutor::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(_mu);
            _cv.wait(lk, [this] { return _stop || !_q.empty(); });
            if (_stop && _q.empty()) return;
            task = std::move(_q.front());
            _q.pop_front();
        }
        // 任务异常不得杀死工作线程（异常语义由上层两阶段 API 承载）
        try { task(); } catch (...) { /* swallow: 上层 done 已捕获业务异常 */ }
    }
}

TaskExecutor& default_task_executor() {
    static size_t threads = []() {
        const char* e = std::getenv("MYFRAME_ASYNC_THREADS");
        int n = e ? std::atoi(e) : 0;
        return (size_t)(n > 0 ? n : 4);
    }();
    static size_t queue = []() {
        const char* e = std::getenv("MYFRAME_ASYNC_QUEUE");
        int n = e ? std::atoi(e) : 0;
        return (size_t)(n > 0 ? n : 1024);
    }();
    static TaskExecutor exec(threads, queue);
    return exec;
}

} // namespace myframe
