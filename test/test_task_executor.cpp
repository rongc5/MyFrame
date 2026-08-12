// test_task_executor.cpp · TaskExecutor 契约回归
//   - 提交的任务会执行；有界队列满则 submit 返回 false（拒绝）；
//   - 任务抛异常不杀死工作线程；shutdown 后拒绝提交。
// 对应：fix(http): safe async lifetime（1b 执行器）
#include "task_executor.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void wait_until(std::atomic<int>& v, int target) {
    for (int i = 0; i < 200 && v.load() < target; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

int main() {
    using namespace myframe;

    // 1) 提交的任务都会执行
    {
        TaskExecutor ex(2, 16);
        std::atomic<int> cnt(0);
        for (int i = 0; i < 8; ++i) CHECK(ex.submit([&cnt]{ ++cnt; }));
        wait_until(cnt, 8);
        CHECK(cnt.load() == 8);
    }

    // 2) 有界队列满 → 拒绝
    {
        TaskExecutor ex(1, 2);          // 1 线程 + 队列上限 2
        std::atomic<bool> block(true);
        std::atomic<int> ran(0);
        ex.submit([&]{ while (block.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); ++ran; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));   // 让唯一线程取走并阻塞在第 1 个
        CHECK(ex.submit([&ran]{ ++ran; }));        // 队列 1
        CHECK(ex.submit([&ran]{ ++ran; }));        // 队列 2
        CHECK(ex.submit([&ran]{ ++ran; }) == false); // 满 → 拒绝
        block.store(false);
        wait_until(ran, 3);
        CHECK(ran.load() == 3);
    }

    // 3) 任务抛异常不杀死工作线程
    {
        TaskExecutor ex(1, 8);
        std::atomic<int> after(0);
        ex.submit([]{ throw std::runtime_error("boom"); });
        ex.submit([&after]{ ++after; });
        wait_until(after, 1);
        CHECK(after.load() == 1);
    }

    // 4) shutdown 后拒绝提交
    {
        TaskExecutor ex(1, 8);
        ex.shutdown();
        CHECK(ex.submit([]{}) == false);
    }

    if (g_fail == 0) { printf("test_task_executor: ALL PASS\n"); return 0; }
    printf("test_task_executor: %d FAIL\n", g_fail);
    return 1;
}
