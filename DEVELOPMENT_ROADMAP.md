# Broadcaster 开发路线图

## 概述

本文档描述 Broadcaster 高性能 TCP 流广播服务的开发规划。项目目标是在单线程模式下实现 40Gbps 吞吐量，支持 1 个发送者和 16 个接收者。

## 开发阶段

```
Phase 1 ──► Phase 2 ──► Phase 3 ──► Phase 4 ──► Phase 5 ──► Phase 6
  基础        RAII       核心        高级        性能        生产
  设施        封装       广播        特性        优化        就绪
 (完成)      (完成)
```

---

## Phase 1: 基础设施 ✅ 已完成

**目标**: 建立项目基础架构

### 已完成任务

- [x] CMake 构建系统配置
- [x] C++23 标准设置
- [x] liburing 依赖集成
- [x] 基础 io_uring 事件循环
- [x] TCP socket 创建与监听
- [x] 异步 accept/send/close 操作

### 交付物

- `CMakeLists.txt` - 构建配置
- `src/main.cpp` - Hello World 服务器

---

## Phase 2: RAII 资源封装 ✅ 已完成

**目标**: 使用 RAII 模式封装系统资源

### 已完成任务

- [x] `FileDescriptor` 类 - 文件描述符包装器
- [x] `Socket` 类 - TCP socket 包装器
- [x] `IoUring` 类 - io_uring 包装器
- [x] 资源生命周期管理
- [x] 移动语义支持

### 交付物

- RAII 类实现在 `src/main.cpp`
- Hello World 服务器验证

---

## Phase 3: 核心广播功能

**目标**: 实现基础的一对多广播

### 3.1 连接管理

```
任务:
├── 实现 ConnectionManager 类
│   ├── 连接角色分配 (Sender/Receiver)
│   ├── 连接状态跟踪
│   └── 连接生命周期管理
├── 实现发送者识别
│   ├── 首连接成为发送者
│   └── 发送者断开后角色重分配
└── 实现接收者管理
    ├── 接收者列表维护
    ├── 动态添加/移除
    └── 最大连接数限制
```

**关键文件**:
- `src/connection_manager.hpp`
- `src/connection_manager.cpp`

### 3.2 环形缓冲区

```
任务:
├── 实现 RingBuffer 类
│   ├── 固定大小内存分配 (256MB)
│   ├── 环形写入逻辑
│   └── 位置追踪（单调递增）
├── 读取接口
│   ├── 获取可读数据范围
│   ├── 支持多读者独立读取
│   └── 落后检测
└── 性能优化
    ├── 内存对齐
    └── 缓存友好的布局
```

**关键文件**:
- `src/ring_buffer.hpp`
- `src/ring_buffer.cpp`

### 3.3 基础广播逻辑

```
任务:
├── 从发送者接收数据
│   ├── IORING_OP_RECV 操作
│   └── 数据写入 RingBuffer
├── 向接收者发送数据
│   ├── IORING_OP_SEND 操作
│   └── 更新读取位置
└── 事件循环集成
    ├── 处理接收完成
    ├── 触发广播
    └── 处理发送完成
```

**关键文件**:
- `src/broadcaster.hpp`
- `src/broadcaster.cpp`

### 3.4 里程碑验证

- [ ] 单发送者连接并发送数据
- [ ] 多接收者接收相同数据
- [ ] 接收者动态加入正常工作
- [ ] 基础吞吐量测试 (目标: 1Gbps)

---

## Phase 4: 高级 io_uring 特性

**目标**: 应用高级特性提升性能

### 4.1 Multishot Accept

```
任务:
├── 替换单次 accept 为 multishot
│   ├── io_uring_prep_multishot_accept
│   └── IORING_CQE_F_MORE 标志处理
├── 错误恢复
│   └── multishot 终止后重新提交
└── 测试验证
    └── 高频连接场景
```

### 4.2 Provided Buffers

```
任务:
├── 实现 ProvidedBufferRing 类
│   ├── 缓冲区环初始化
│   ├── 缓冲区注册到 io_uring
│   └── 缓冲区回收机制
├── 集成到接收流程
│   ├── IORING_OP_RECV 使用 provided buffer
│   └── 接收完成后回收缓冲区
└── 配置优化
    ├── 缓冲区大小调优 (64KB)
    └── 缓冲区数量调优 (64个)
```

**关键文件**:
- `src/buffer_ring.hpp`
- `src/buffer_ring.cpp`

### 4.3 Zero-Copy Send

```
任务:
├── 实现零拷贝发送
│   ├── io_uring_prep_send_zc
│   └── 通知处理 (IORING_CQE_F_NOTIF)
├── 缓冲区生命周期管理
│   └── 确保发送完成前数据有效
└── 回退机制
    └── 内核不支持时使用普通 send
```

### 4.4 批量操作优化

```
任务:
├── 批量 SQE 提交
│   ├── 累积多个操作
│   └── 批量 io_uring_submit
├── 批量 CQE 处理
│   ├── io_uring_peek_batch_cqe
│   └── io_uring_cq_advance
└── CQE 跳过优化
    └── IOSQE_CQE_SKIP_SUCCESS
```

### 4.5 里程碑验证

- [ ] Multishot accept 正常工作
- [ ] Provided buffers 零拷贝接收
- [ ] Zero-copy send 正常工作
- [ ] 吞吐量提升 (目标: 10Gbps)

---

## Phase 5: 背压与稳定性

**目标**: 处理接收者落后和系统稳定性

### 5.1 背压控制

```
任务:
├── 实现 BackpressureController 类
│   ├── 落后阈值配置
│   ├── 落后检测逻辑
│   └── 断开决策
├── 接收者落后处理
│   ├── 检测落后接收者
│   ├── 优雅断开连接
│   └── 清理资源
└── 发送者流控
    └── RingBuffer 满时暂停接收
```

**关键文件**:
- `src/backpressure.hpp`
- `src/backpressure.cpp`

### 5.2 错误处理增强

```
任务:
├── 错误分类系统
│   ├── 可恢复错误
│   ├── 连接错误
│   └── 系统错误
├── 错误恢复策略
│   ├── 重试机制
│   ├── 资源清理
│   └── 日志记录
└── 优雅关闭
    ├── 信号处理 (SIGTERM, SIGINT)
    └── 资源清理
```

### 5.3 资源限制

```
任务:
├── 连接数限制
│   └── 拒绝超出限制的连接
├── 内存使用监控
│   └── 缓冲区使用统计
└── 操作队列限制
    └── SQE 队列深度管理
```

### 5.4 里程碑验证

- [ ] 落后接收者被正确断开
- [ ] 系统在压力下保持稳定
- [ ] 错误情况正确处理
- [ ] 优雅关闭工作正常

---

## Phase 6: 性能调优与生产就绪

**目标**: 达到 40Gbps 目标吞吐量

### 6.1 性能分析

```
任务:
├── 性能指标收集
│   ├── 吞吐量统计
│   ├── 延迟分布
│   └── CPU 使用率
├── 瓶颈识别
│   ├── perf 分析
│   ├── 火焰图生成
│   └── 系统调用追踪
└── 基准测试套件
    ├── 吞吐量测试
    ├── 延迟测试
    └── 稳定性测试
```

### 6.2 代码优化

```
任务:
├── 热点优化
│   ├── 关键路径内联
│   ├── 分支预测优化
│   └── 循环优化
├── 内存访问优化
│   ├── 缓存行对齐
│   ├── 预取指令
│   └── 减少 false sharing
└── 编译器优化
    ├── LTO 启用
    ├── PGO 优化
    └── SIMD 利用
```

### 6.3 系统调优

```
任务:
├── 网络参数优化
│   ├── TCP 缓冲区大小
│   ├── 拥塞控制算法 (BBR)
│   └── 中断合并
├── io_uring 参数优化
│   ├── 队列深度
│   ├── 标志位优化
│   └── SQPOLL 模式评估
└── CPU 亲和性
    ├── 进程绑定核心
    └── 中断亲和性
```

### 6.4 监控与可观测性

```
任务:
├── 指标导出
│   ├── 实时统计接口
│   └── Prometheus 兼容格式
├── 日志系统
│   ├── 结构化日志
│   └── 日志级别控制
└── 健康检查
    ├── 存活检查接口
    └── 就绪检查接口
```

### 6.5 文档与测试

```
任务:
├── 使用文档
│   ├── 部署指南
│   ├── 配置说明
│   └── 调优指南
├── 测试覆盖
│   ├── 单元测试
│   ├── 集成测试
│   └── 压力测试
└── 性能基准
    └── 标准化性能报告
```

### 6.6 里程碑验证

- [ ] 达到 40Gbps 吞吐量
- [ ] 16 接收者同时工作
- [ ] 长时间稳定运行 (24小时+)
- [ ] 文档和测试完备

---

## 文件结构规划

```
Broadcaster/
├── CMakeLists.txt
├── CLAUDE.md
├── ARCHITECTURE.md
├── DEVELOPMENT_ROADMAP.md
├── PROTOCOL_DESIGN.md
├── README.md
├── src/
│   ├── main.cpp                 # 程序入口
│   ├── broadcaster.hpp          # 广播器主类
│   ├── broadcaster.cpp
│   ├── io_uring_wrapper.hpp     # io_uring RAII 封装
│   ├── io_uring_wrapper.cpp
│   ├── socket.hpp               # Socket RAII 封装
│   ├── socket.cpp
│   ├── ring_buffer.hpp          # 环形缓冲区
│   ├── ring_buffer.cpp
│   ├── connection_manager.hpp   # 连接管理
│   ├── connection_manager.cpp
│   ├── buffer_ring.hpp          # Provided buffers
│   ├── buffer_ring.cpp
│   ├── backpressure.hpp         # 背压控制
│   ├── backpressure.cpp
│   ├── metrics.hpp              # 指标收集
│   └── metrics.cpp
├── tests/
│   ├── test_ring_buffer.cpp
│   ├── test_connection_manager.cpp
│   └── test_broadcaster.cpp
├── benchmarks/
│   ├── throughput_test.cpp
│   └── latency_test.cpp
└── scripts/
    ├── tune_system.sh           # 系统调优脚本
    └── run_benchmark.sh         # 基准测试脚本
```

---

## 技术决策记录

### TD-001: 单线程 vs 多线程

**决策**: 单线程模式

**理由**:
- io_uring 在单线程下已能充分利用 CPU
- 避免锁竞争和线程同步开销
- 简化代码复杂度
- 40Gbps 在单核下可实现

### TD-002: 环形缓冲区大小

**决策**: 256MB

**理由**:
- 5GB/s × 50ms = 256MB
- 允许接收者有约 50ms 的处理延迟
- 内存占用可接受

### TD-003: 发送者识别策略

**决策**: 首连接成为发送者

**理由**:
- 实现简单
- 无需额外握手协议
- 适合固定拓扑场景

### TD-004: Zero-Copy 策略

**决策**: 同时使用 provided buffers 和 send_zc

**理由**:
- 接收端: provided buffers 减少拷贝
- 发送端: send_zc 直接从 ring buffer 发送
- 最大化减少数据拷贝

---

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 内核版本不兼容 | 高 | 功能检测 + 回退实现 |
| 网卡驱动瓶颈 | 高 | 推荐支持的网卡型号 |
| 内存不足 | 中 | 可配置的缓冲区大小 |
| 发送者断开 | 中 | 自动重新分配角色 |
| 所有接收者落后 | 低 | 监控告警 |

---

## 验收标准

### 功能验收

- [ ] 单发送者能够连接并发送数据
- [ ] 16 个接收者能够同时接收数据
- [ ] 接收者可以动态加入和退出
- [ ] 落后的接收者被自动断开
- [ ] 发送者断开后系统正常处理

### 性能验收

- [ ] 总吞吐量达到 40Gbps
- [ ] 单接收者延迟 < 1ms (P99)
- [ ] CPU 使用率 < 100% (单核)
- [ ] 内存使用稳定，无泄漏

### 稳定性验收

- [ ] 24 小时连续运行无崩溃
- [ ] 各种错误场景正确处理
- [ ] 优雅关闭正常工作
