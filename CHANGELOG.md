# Changelog

本文件记录 myframe(产出 `libmyframe.a`)的版本变更。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/),版本遵循 [SemVer](https://semver.org/lang/zh-CN/)。

版本号见根目录 `VERSION` 文件(第一行 `myframe <版本>`,供 `scripts/pack_myframe.sh` 解析)。

## [1.0.0] - 2026-06-22

首个带版本号的发布。确立 `VERSION` / `CHANGELOG.md` / `scripts/pack_myframe.sh` 版本与打包流程。

### Fixed
- **跨线程通道唤醒丢失导致消费者线程永久空转(根因修复)。**
  原 `channel_data_process` 的"合并唤醒(coalesced wake)"配合 EPOLLET 边沿触发,会在
  低频通道(如每 20~30s 才来一条消息的工作线程)上丢唤醒 —— 消息入队后没有任何东西
  把消费者从 `epoll_wait` 叫醒,线程永久 idle,积压消息永不处理。
  - 改为 **rxlogger 模型**:`base_net_thread::run()` 每轮无条件遍历 `_channel_msg_vec`
    调 `channel_data_process::on_tick()` 排空队列;`obj_process` 里的 `epoll_wait` 带超时,
    故即便唤醒字节全部丢失,积压消息也会在一个超时内(≤50ms)被处理。
  - 唤醒字节降级为**纯延迟优化**(coalesced,管道恒 ≤1 字节),`MSG_DONTWAIT` 保证
    生产者绝不阻塞;缓冲满(EAGAIN)良性,不再刷错误日志。
- **`base_net_thread::put_obj_msg` 的 use-after-free。**
  原实现无锁 `resolve_thread()` 返回裸指针后在锁外 `put_msg`,目标线程可能在此期间析构。
  改为全程持 `_thread_map_mutex` 覆盖 lookup + `put_msg`(与析构在同一把锁下摘除自身互斥),
  消除并发析构窗口。
- **唤醒字节写入字节数。**
  原用 `send(fd, CHANNEL_MSG_TAG, sizeof(CHANNEL_MSG_TAG), ...)`,而 `CHANNEL_MSG_TAG = "c"`
  的 `sizeof` 为 2(含 `'\0'`)。改为显式写 1 字节并按 `ret != 1` 判错,避免 partial-send 误判。

### Added
- `channel_data_process::on_tick()` / `drain_queue()`:每轮无条件排空队列(正确性保证)。
- `base_data_process::on_tick()`:默认空操作的每-tick 钩子(普通连接无影响)。
- **批处理预算**:`drain_queue` 单轮最多处理 `kMaxBatchPerTick`(1024)条,剩余留队列、
  保持武装并写一个唤醒字节让下一轮 `epoll_wait` 立刻返回继续排 —— 避免慢 handler 用一个
  超大 batch 长时间霸占 net 线程,缓解线程级 tail latency。
- 第二道兜底保留:watchdog 定时器(`process_recv_buf` 首次唤醒时装上,检测队列积压+超时
  → `set_real_net(true)` 经 `obj_process` 再处理一次)。
- 回归测试 `test/test_channel_wakeup.cpp`:低频 / 突发 / 长空闲后单条 / 慢消费者洪泛,
  验证零丢失、生产者不阻塞、无唤醒字节也能交付。

### Notes
- 持锁覆盖 `put_obj_msg` 会使跨线程投递在 `_thread_map_mutex` 上串行(正确性优先);
  本项目消息频率下开销可接受。
