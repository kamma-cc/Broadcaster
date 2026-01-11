# CODEX_ROADMAP.md

以下路线图从 0 设计，针对需求：
单发送者 + 多接收者 TCP 广播、动态加入、原样转发、40Gbps，
采用 C++ 与最新 Linux 内核（io_uring）。

## 目标与约束

- 单线程 io_uring 事件循环，优先跑满 40Gbps 网卡。
- 发送者 TCP 流原样广播给所有接收者，不做重编码。
- 接收者可随时加入/退出，仅接收加入后的数据。
- RAII 管理资源，避免泄漏。

## Phase 0: 需求与策略确认

- 明确 sender 断开后角色策略（清理接收者或保留等待新 sender）。
- 明确 receiver 上限（默认 16）与落后断开阈值。
- 明确背压策略（ring buffer 满时的处理行为）。

## Phase 1: 基础设施与可运行原型

- CMake + C++23 + liburing 接入。
- io_uring 初始化与事件循环。
- TCP listen + async accept。
- 最小可运行 server（验证连通性与 io_uring 流程）。

## Phase 2: RAII 与模块化

- FileDescriptor / Socket / IoUring RAII 封装。
- 将核心功能拆分为独立模块文件。
- main 只保留启动与事件循环入口。

## Phase 3: 连接管理

- 首连接为 sender，其它为 receiver。
- receiver 列表管理与上限控制。
- sender 断开后的重分配逻辑与状态复位。

## Phase 4: 环形缓冲区

- 256MB 固定大小 ring buffer。
- 单调写指针 + 每个 receiver 独立读指针。
- 可读区间 API，用于零拷贝发送。
- 落后检测（max lag）与断开策略。

## Phase 5: 基础广播

- sender recv -> 写入 ring buffer。
- receiver send <- ring buffer 可读区间。
- send 完成后更新读指针。
- receiver 落后断开。

## Phase 6: 高性能 io_uring 特性

- multishot accept。
- provided buffers recv。
- send_zc 零拷贝发送 + 运行时回退。
- 批量 submit 与批量 cqe 处理。

## Phase 7: 背压与稳定性

- ring buffer 满时策略（暂停 recv 或丢弃/断开）。
- 错误分类与恢复（EAGAIN/EINTR/连接错误）。
- 优雅退出（SIGINT/SIGTERM）。

## Phase 8: 性能验证与调优

- 吞吐与延迟指标统计。
- 基准压测脚本（逐步到 40Gbps）。
- 系统调优建议（TCP 缓冲区、BBR、队列深度）。

## 验收标准

- 功能：1 sender + 多 receiver，动态加入可用。
- 正确性：原样转发，无历史数据回放。
- 性能：接近 40Gbps，总吞吐稳定。
