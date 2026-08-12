// task_executor.h · 进程级阻塞任务线程池（供 HTTP 两阶段 async 使用）
// 契约：固定线程数、有界队列、队列满则 submit 返回 false（拒绝→上层 503）、优雅停机。
// 注意：不是网络线程池（base_net_thread 是网络线程）；这是业务阻塞任务池。
#ifndef __MYFRAME_TASK_EXECUTOR_H__
#define __MYFRAME_TASK_EXECUTOR_H__

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <cstddef>
#include <atomic>

namespace myframe {

class TaskExecutor {
public:
    TaskExecutor(size_t threads, size_t max_queue);
    ~TaskExecutor();

    // 提交任务；队列满或已停机返回 false（拒绝，不阻塞、不新建线程）
    bool submit(std::function<void()> task);

    // 停止接收、唤醒并 join 所有工作线程（剩余队列任务被丢弃）
    void shutdown();

    size_t queue_size() const;

private:
    void worker_loop();

    mutable std::mutex _mu;
    std::condition_variable _cv;
    std::deque<std::function<void()> > _q;
    std::vector<std::thread> _workers;
    size_t _max_queue;
    bool _stop;
};

// 进程级共享执行器（函数局部静态，非裸全局变量；线程数取 env MYFRAME_ASYNC_THREADS，默认 4）。
// 后续可改为由 server/factory 注入。
TaskExecutor& default_task_executor();

} // namespace myframe

#endif
