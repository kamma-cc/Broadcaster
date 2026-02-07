# Broadcaster 开发路线图（重制版）

> 更新日期：2026-02-07  
> 基线代码状态：`Phase 1/2` 已完成（单文件 `src/main.cpp` 中具备 io_uring + RAII Hello World 服务器）

## 1. 目标与边界

### 1.1 北极星目标

- 单线程 io_uring 架构下，实现 `1 Sender + 16 Receivers` 的 TCP 原样广播。
- 接收者支持动态加入/退出，只接收加入后的新数据。
- 在目标环境下达到 `40Gbps` 总吞吐（入站 + 出站合计），并保持稳定运行。

### 1.2 非目标（当前版本不做）

- 多发送者并发写入
- 消息持久化与回放
- 内容过滤/路由
- 跨节点分布式扩展

### 1.3 设计约束

- 优先使用原生 `liburing`，避免高层网络框架。
- 单线程事件循环，不引入锁竞争。
- Broadcaster 仅做字节搬运，不做编码/解码/重分帧。
- Sender/Receiver 使用双端口接入，避免角色误判。
- 关键路径优先围绕 `RingBuffer + Backpressure + Zero-Copy` 优化。
- 以可测量指标作为每阶段退出条件（DoD）。

## 2. 当前状态评估（2026-02-07）

### 已具备能力

- CMake + C++23 + liburing 构建链路可用。
- `FileDescriptor` / `Socket` / `IoUring` RAII 包装已实现。
- 异步 `accept/send/close` 已打通，具备最小事件循环能力。

### 主要缺口

- 代码仍集中在 `src/main.cpp`，缺乏模块边界。
- 尚无 Sender/Receiver 状态机、RingBuffer、广播主链路。
- 尚无背压控制、错误恢复、观测指标、压测基线。
- 尚未使用 multishot/provided buffers/send_zc 等关键 io_uring 特性。

## 3. 里程碑总览

| 阶段 | 目标 | 预计时长 | 关键退出标准 |
| --- | --- | --- | --- |
| Phase 0 | 工程化重构与测试底座 | 1 周 | 模块拆分完成，冒烟测试稳定 |
| Phase 1 | 连接模型与 RingBuffer | 1-2 周 | 角色分配 + 数据结构测试通过 |
| Phase 2 | 广播主链路 MVP | 2 周 | 1 Sender + 4 Receivers 稳定广播，吞吐 >= 1Gbps |
| Phase 3 | 高级 io_uring 能力接入 | 2 周 | multishot + provided buffers 可用，吞吐 >= 10Gbps |
| Phase 4 | Zero-Copy 与稳定性强化 | 2 周 | send_zc 路径稳定，吞吐 >= 20Gbps |
| Phase 5 | 40Gbps 性能冲刺 | 2-3 周 | 16 Receivers 下达到 40Gbps 目标 |
| Phase 6 | 生产就绪与交付 | 1 周 | 文档、配置、监控、回归测试完备 |

> 说明：`PROTOCOL_DESIGN.md` 中 COBS 分帧能力作为并行可选赛道（Track B），不阻塞主线性能目标。

## 4. 分阶段实施计划

## Phase 0：工程化重构与测试底座（1 周）

### 目标

将当前单文件实现重构为可扩展模块，建立最小自动化验证能力。

### 核心任务

- 拆分 `src/main.cpp` 至规划模块：
  - `src/io_uring_wrapper.hpp/.cpp`
  - `src/socket.hpp/.cpp`
  - `src/connection_manager.hpp/.cpp`
  - `src/ring_buffer.hpp/.cpp`
  - `src/broadcaster.hpp/.cpp`
- 统一 CQE 用户数据结构（操作类型、fd、连接 id、buffer id）。
- 建立基础测试骨架（`tests/` + CTest），至少覆盖：
  - RAII 资源释放
  - RingBuffer 基本读写语义（可先用 stub）
- 增加基础脚本：
  - `scripts/run_smoke.sh`
  - `scripts/run_local_bench.sh`（先保留空实现也可）

### 退出标准（DoD）

- Debug/Release 均可构建。
- 至少 3 个基础测试通过并进入 CI（若暂无 CI，先在本地脚本化）。
- 主程序入口仅保留参数解析 + 启动逻辑。

## Phase 1：连接模型与 RingBuffer（1-2 周）

### 目标

建立清晰的连接角色模型与数据缓冲核心结构。

### 核心任务

- 实现连接角色分配：
  - 监听双端口：`sender_port` / `receiver_port`
  - `sender_port` 仅允许单 Sender
  - `receiver_port` 接入多 Receiver（上限 16）
- 实现 `ReceiverState` 管理：
  - 独立读指针
  - pending/send_in_progress 状态
  - 最大连接数限制（默认 16）
- 实现 `RingBuffer`（默认 256MB）：
  - 单调写指针
  - 可读区间查询
  - 落后判定接口

### 退出标准（DoD）

- 连接状态机单元测试通过。
- RingBuffer 读写、回绕、落后检测测试通过。
- Sender/Receiver 生命周期在日志中可观察。

## Phase 2：广播主链路 MVP（2 周）

### 目标

打通“接收 Sender 数据 -> 写 RingBuffer -> 广播 Receiver”的完整闭环。

### 核心任务

- Sender 收包（初版可用普通 `recv` / `IORING_OP_RECV`）。
- 实现 `DrainMode`：无 Receiver 时持续读取 Sender 并丢弃数据。
- 数据写入 RingBuffer，并触发 Receiver 发送调度。
- Receiver 发送完成后推进读指针。
- 实现可配置背压参数：
  - `soft_lag_bytes`
  - `hard_lag_bytes`
  - `kick_grace_ms`
  - `pause_watermark_pct`
  - `resume_watermark_pct`
- 完成动态加入/退出行为验证。

### 退出标准（DoD）

- 功能正确性：
  - 1 Sender + 4 Receivers 场景下内容一致。
  - 新加入 Receiver 不回放历史数据。
  - 落后 Receiver 可被自动断开。
  - 无 Receiver 时服务持续读取且累计丢弃指标。
- 性能基线：
  - 单机本地压测吞吐 >= 1Gbps。

## Phase 3：高级 io_uring 能力接入（2 周）

### 目标

引入降低系统调用与拷贝开销的关键 io_uring 特性。

### 核心任务

- 接入 `multishot accept` 并处理 `IORING_CQE_F_MORE` 生命周期。
- 接入 provided buffers（buffer ring 初始化、归还、复用）。
- 引入批量提交与批量 CQE 处理：
  - `io_uring_submit` 批量化
  - `io_uring_peek_batch_cqe` 路径
- 建立特性探测与降级机制（内核不支持时自动回退）。

### 退出标准（DoD）

- 16 Receiver 频繁连接/断开场景稳定运行 30 分钟。
- 吞吐达到 >= 10Gbps（实验环境）。
- 无明显 buffer 泄漏或提交队列饥饿。

## Phase 4：Zero-Copy 与稳定性强化（2 周）

### 目标

完成发送侧 zero-copy 路径，并补齐容错与可观测性。

### 核心任务

- 接入 `IORING_OP_SEND_ZC`，处理通知 CQE。
- 建立 zero-copy 与普通 send 的双路径回退策略。
- 完善错误分类与恢复：
  - `EAGAIN/EINTR` 重试
  - 连接错误清理
  - 缓冲区耗尽保护
- 增加运行时指标：
  - 收发字节、活跃连接、断开原因、buffer 使用率

### 退出标准（DoD）

- send_zc 路径与回退路径均有覆盖测试。
- 6 小时 soak test 无崩溃、无明显内存增长。
- 吞吐达到 >= 20Gbps。

## Phase 5：40Gbps 性能冲刺（2-3 周）

### 目标

在目标硬件环境冲刺并固化 40Gbps 能力。

### 核心任务

- 建立标准化压测拓扑与脚本（固定消息大小、连接模型、时长）。
- 使用 `perf`/火焰图定位热点并迭代优化：
  - CQE 处理路径
  - RingBuffer 访存
  - 批量度参数
- 系统参数调优并沉淀脚本：
  - `net.core.rmem_max/wmem_max`
  - `tcp_rmem/tcp_wmem`
  - 拥塞控制（BBR）与队列参数

### 退出标准（DoD）

- 在目标环境实现 16 Receivers 下总吞吐 >= 40Gbps。
- P99 延迟 < 1ms（固定测试口径）。
- 单核 CPU 使用率可控且无长期退化。

## Phase 6：生产就绪与交付（1 周）

### 目标

完成可部署、可观测、可回归的发布基线。

### 核心任务

- 配置化参数（端口、buffer 大小、连接上限、lag 阈值）。
- 完整运行文档：
  - 部署指南
  - 内核要求与兼容矩阵
  - 性能调优手册
- 监控与日志：
  - 结构化日志
  - 指标导出（文本或 Prometheus）
- 回归测试清单与发布检查项。

### 退出标准（DoD）

- 24 小时稳定性测试通过。
- 发布文档可被新成员独立复现部署与压测。
- 核心故障场景具备可观测信号与处置说明。

## 5. Track B：COBS 协议赛道（可选并行）

`PROTOCOL_DESIGN.md` 定义了“中途加入快速同步”的 COBS 方案。该能力在当前版本定位为可选增强，不阻塞主线。

### 触发条件

- 业务明确需要“消息边界语义”而非纯字节流转发。

### 并行任务

- 在上游 Producer 或下游 Consumer/网关实现 COBS 编解码。
- 评估 COBS 对吞吐与延迟的影响，并给出默认策略。

### 成功标准

- Broadcaster 主路径保持 passthrough（不改数据）。
- 增量开销可量化且可接受（默认目标 <= 3% CPU 增幅）。

## 6. 风险与缓解

| 风险 | 影响 | 缓解策略 |
| --- | --- | --- |
| 内核/驱动特性缺失（send_zc、multishot） | 高 | 启动时探测 + 运行时回退路径 |
| 压测环境与目标环境差异大 | 高 | 固化基准拓扑，区分实验结论与生产结论 |
| Receiver 慢消费导致全局抖动 | 中 | 严格 lag 断开 + 指标告警 |
| 单线程热点过于集中 | 中 | 先算法/内存布局优化，再评估多核扩展 |
| 代码重构阶段引入回归 | 中 | Phase 0 先建测试底座，分批迁移 |

## 7. 阶段验收总清单

- 功能验收：
  - 1 Sender + 16 Receivers 正常广播
  - 动态加入/退出正确
  - 落后 Receiver 自动断开
- 性能验收：
  - 总吞吐 >= 40Gbps
  - P99 延迟 < 1ms
  - 单线程模型稳定
- 稳定性验收：
  - 24 小时稳定运行
  - 异常场景（断连、缓冲区压力、特性回退）可恢复

## 8. 下一步执行建议（最近两周）

1. 完成 `Phase 0`：先做模块拆分与测试底座，不直接上高级特性。  
2. 并行推进 `Phase 1` 的 `ConnectionManager + RingBuffer`，把核心数据结构先稳定。  
3. 以 `Phase 2` 的 `1Gbps MVP` 作为第一个硬门槛，达成后再进入 io_uring 高级特性阶段。
