# Broadcaster 架构设计

## 1. 系统概述

Broadcaster 是一个高性能的 TCP 流广播服务，设计目标是在单线程模式下实现 40Gbps 的吞吐量。系统采用 io_uring 作为核心 I/O 引擎，充分利用 Linux 内核的异步 I/O 能力。

### 1.1 核心功能

```
                    ┌─────────────────────────────────────┐
                    │           Broadcaster               │
                    │                                     │
   ┌─────────┐      │  ┌─────────────────────────────┐   │      ┌──────────┐
   │ Sender  │─────►│  │      Ring Buffer            │   │─────►│Receiver 1│
   │ (1个)   │ TCP  │  │  ┌───┬───┬───┬───┬───┬───┐  │   │ TCP  └──────────┘
   └─────────┘      │  │  │ A │ B │ C │ D │ E │ F │  │   │      ┌──────────┐
                    │  │  └───┴───┴───┴───┴───┴───┘  │   │─────►│Receiver 2│
                    │  │         ▲                    │   │      └──────────┘
                    │  │    写入位置                   │   │           ⋮
                    │  └─────────────────────────────┘   │      ┌──────────┐
                    │                                     │─────►│Receiver N│
                    └─────────────────────────────────────┘      └──────────┘
```

- **单发送者**: 一个 TCP 连接作为数据源
- **多接收者**: 最多 16 个接收者同时接收数据
- **双端口接入**: Sender 与 Receiver 使用独立监听端口
- **原样转发**: 发送者的 TCP 流原封不动地广播给所有接收者
- **零修改原则**: Broadcaster 不做编码/解码/重分帧/重排
- **动态加入**: 接收者可随时加入/退出，只接收加入后的数据
- **落后断开**: 当接收者处理速度落后时，主动断开连接
- **无接收者排空**: 当没有接收者时，继续读取发送者并丢弃数据

### 1.2 性能目标

| 指标 | 目标值 |
|------|--------|
| 总吞吐量 | 40 Gbps (发送+接收合计) |
| 接收者数量 | 16 个 |
| 单接收者带宽 | ~2.35 Gbps |
| 线程模型 | 单线程 |
| CPU 核心 | 1 核 |

### 1.3 带宽计算

假设发送者速率为 R：
- 发送者入站: R
- 接收者出站: 16 × R
- 总带宽: 17R = 40 Gbps
- 发送者速率 R ≈ 2.35 Gbps

## 2. 核心架构

### 2.1 组件架构

```
┌──────────────────────────────────────────────────────────────────┐
│                         Application Layer                         │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐  │
│  │   Sender   │  │  Receiver  │  │   Ring     │  │  Backpres- │  │
│  │  Manager   │  │  Manager   │  │   Buffer   │  │   sure     │  │
│  └────────────┘  └────────────┘  └────────────┘  └────────────┘  │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│                         io_uring Layer                            │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐  │
│  │  Multishot │  │  Provided  │  │  Zero-Copy │  │   Batch    │  │
│  │   Accept   │  │  Buffers   │  │    Send    │  │   Submit   │  │
│  └────────────┘  └────────────┘  └────────────┘  └────────────┘  │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│                         Linux Kernel                              │
│              io_uring + TCP Stack + Network Driver                │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 SenderManager (发送者管理器)

负责管理唯一的发送者连接：

```cpp
class SenderManager {
    Socket sender_socket_;           // 发送者连接
    bool has_sender_ = false;        // 是否已有发送者

public:
    // 来自 sender_port 的连接绑定为发送者（若已存在发送者则返回 false）
    bool attach_sender(int fd);

    // 检查发送者是否存在
    bool has_sender() const;

    // 发送者断开时调用
    void detach_sender();
};
```

#### 2.2.2 ReceiverManager (接收者管理器)

管理多个接收者连接及其状态：

```cpp
class ReceiverManager {
    struct ReceiverState {
        int fd;
        size_t read_position;        // 在环形缓冲区中的读取位置
        size_t pending_bytes;        // 待发送字节数
        bool send_in_progress;       // 是否有发送操作进行中
        uint64_t last_progress_ns;   // 最近一次发送推进时间
    };

    std::array<ReceiverState, MAX_RECEIVERS> receivers_;
    size_t active_count_ = 0;

public:
    // 添加新接收者（从当前写入位置开始）
    bool add_receiver(int fd, size_t current_write_pos);

    // 移除接收者
    void remove_receiver(int fd);

    // 检查并断开落后的接收者
    void check_lagging_receivers(size_t write_pos, size_t max_lag);
};
```

#### 2.2.3 RingBuffer (环形缓冲区)

高性能的环形缓冲区，用于存储待广播的数据：

```cpp
class RingBuffer {
    std::unique_ptr<uint8_t[]> buffer_;
    size_t capacity_;                // 缓冲区大小 (建议 64MB - 256MB)
    size_t write_pos_ = 0;           // 写入位置（单调递增）

public:
    // 写入数据（返回实际写入的字节数）
    size_t write(const uint8_t* data, size_t len);

    // 获取可读数据的指针和长度（用于 zero-copy send）
    std::pair<const uint8_t*, size_t> get_readable(size_t read_pos) const;

    // 检查读取位置是否已落后太多
    bool is_lagging(size_t read_pos, size_t max_lag) const;
};
```

#### 2.2.4 BackpressureController (背压控制器)

管理系统的背压策略：

```cpp
class BackpressureController {
    struct Config {
        size_t soft_lag_bytes = 32ULL * 1024 * 1024;
        size_t hard_lag_bytes = 96ULL * 1024 * 1024;
        uint32_t kick_grace_ms = 500;
        uint8_t pause_watermark_pct = 80;
        uint8_t resume_watermark_pct = 55;
        size_t recv_chunk_bytes = 64 * 1024;
    } cfg_;

public:
    // Receiver 级：是否判定为慢连接
    bool is_slow(size_t lag_bytes) const;
    // Receiver 级：是否需要断开（硬阈值 + 宽限期）
    bool should_disconnect(size_t lag_bytes, uint32_t no_progress_ms) const;
    // Global 级：是否暂停/恢复 Sender recv（带滞回）
    bool should_pause_recv(size_t used_bytes, size_t capacity, size_t free_bytes) const;
    bool should_resume_recv(size_t used_bytes, size_t capacity, size_t free_bytes) const;
};
```

### 2.3 连接角色识别（双端口）

由于需要区分发送者和接收者，采用以下策略：

**方案: 双监听端口**

```cpp
enum class ConnectionRole {
    Sender,     // 发送者：数据源
    Receiver    // 接收者：数据消费者
};

constexpr uint16_t SENDER_PORT = 7000;
constexpr uint16_t RECEIVER_PORT = 7001;

// sender_port: 只接受 Sender，且同一时刻仅允许一个 Sender
// receiver_port: 只接受 Receiver
// Sender 断开后不做角色重分配，等待新的 Sender 连接 sender_port
```

## 3. io_uring 高级特性应用

### 3.1 Multishot Accept

使用多次触发的 accept 操作，避免每次接受连接后重新提交：

```cpp
void submit_multishot_accept(IoUring& ring, int listen_fd, ListenType type) {
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, make_accept_op(type));
}
```

启动时分别为 `sender_port` 与 `receiver_port` 提交 accept。

### 3.2 Provided Buffers (提供的缓冲区)

为接收操作预先注册缓冲区，减少内存拷贝：

```cpp
class ProvidedBufferRing {
    static constexpr size_t BUFFER_SIZE = 64 * 1024;  // 64KB per buffer
    static constexpr size_t BUFFER_COUNT = 64;        // 64 buffers

    io_uring_buf_ring* buf_ring_;
    std::array<uint8_t, BUFFER_SIZE * BUFFER_COUNT> buffer_pool_;

public:
    void setup(IoUring& ring, int bgid);
    void recycle_buffer(int bid);
};
```

### 3.3 Zero-Copy Send

使用 `IORING_OP_SEND_ZC` 实现零拷贝发送：

```cpp
void submit_zerocopy_send(IoUring& ring, int fd,
                          const void* buf, size_t len) {
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_send_zc(sqe, fd, buf, len, 0, 0);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;  // 成功时跳过 CQE
}
```

### 3.4 批量提交与完成

```cpp
class BatchSubmitter {
    IoUring& ring_;
    int pending_sqes_ = 0;
    static constexpr int BATCH_SIZE = 32;

public:
    void add_sqe() {
        pending_sqes_++;
        if (pending_sqes_ >= BATCH_SIZE) {
            flush();
        }
    }

    void flush() {
        if (pending_sqes_ > 0) {
            ring_.submit();
            pending_sqes_ = 0;
        }
    }
};
```

## 4. 数据流设计

### 4.1 BroadcastMode：有接收者时的数据流

```
Sender socket bytes
       │
       ▼
┌─────────────────┐
│  io_uring recv  │◄── Provided Buffer
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Ring Buffer   │──► write_pos 递增
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 调度 Receiver   │
│ send/send_zc    │
└─────────────────┘
```

### 4.2 DrainMode：无接收者时的数据流

```
Sender socket bytes
       │
       ▼
┌─────────────────┐
│  io_uring recv  │
└────────┬────────┘
         │
         ▼
┌────────────────────────────┐
│ 直接丢弃 payload（不入 ring）│
│ bytes_dropped_no_receiver++ │
└────────────────────────────┘
```

### 4.3 发送流程 (到接收者)

```
┌─────────────────┐
│   Ring Buffer   │
│  (读取数据)     │
└────────┬────────┘
         │
         ▼
┌─────────────────────────────────┐
│  io_uring send_zc (zero-copy)  │
│  ┌─────┐ ┌─────┐ ┌─────┐       │
│  │ R1  │ │ R2  │ │ R3  │ ...   │
│  └─────┘ └─────┘ └─────┘       │
└─────────────────────────────────┘
         │
         ▼
┌─────────────────┐
│  更新各接收者   │
│   读取位置      │
└─────────────────┘
```

### 4.4 完整事件循环

```cpp
void event_loop(IoUring& ring, int sender_listen_fd, int receiver_listen_fd) {
    submit_multishot_accept(ring, sender_listen_fd, ListenType::SenderListen);
    submit_multishot_accept(ring, receiver_listen_fd, ListenType::ReceiverListen);

    while (running) {
        // 批量获取完成事件
        io_uring_cqe* cqes[BATCH_SIZE];
        int count = io_uring_peek_batch_cqe(&ring, cqes, BATCH_SIZE);

        for (int i = 0; i < count; i++) {
            handle_completion(cqes[i]);
        }

        io_uring_cq_advance(&ring, count);

        // 如果没有事件，等待
        if (count == 0) {
            io_uring_wait_cqe(&ring, &cqe);
        }

        if (receiver_manager.active_count() == 0) {
            enter_drain_mode();      // 持续 recv 并丢弃
        } else {
            enter_broadcast_mode();  // 写 ring 并广播
        }
    }
}
```

## 5. 背压与落后处理

### 5.1 设计原则

- **不改数据**：只做字节搬运，不做编解码与重排。
- **Receiver 优先治理**：优先识别并断开慢接收者，避免拖垮整体。
- **Global 水位控制**：RingBuffer 高水位暂停 Sender `recv`，低水位恢复（滞回防抖）。
- **无 Receiver 排空**：当接收者数量为 0 时持续读取并丢弃，不写入 RingBuffer。

### 5.2 核心指标定义

```cpp
// 单调位置指针
size_t write_pos;     // RingBuffer 全局写入位置
size_t read_pos_i;    // Receiver i 的读取位置

// 每个 Receiver 的落后量
size_t lag_i = write_pos - read_pos_i;

// 全局占用量（最慢 Receiver 决定）
size_t min_read_pos = min(read_pos_i for all active receivers);
size_t used_bytes = write_pos - min_read_pos;
size_t free_bytes = capacity - used_bytes;
```

### 5.3 可配置参数与默认值

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `soft_lag_bytes` | 32MB | 超过后标记慢连接 |
| `hard_lag_bytes` | 96MB | 超过后进入断开候选 |
| `kick_grace_ms` | 500ms | 超过硬阈值后允许的无进展窗口 |
| `pause_watermark_pct` | 80 | 高水位，触发暂停 Sender recv |
| `resume_watermark_pct` | 55 | 低水位，触发恢复 Sender recv |
| `recv_chunk_bytes` | 64KB | 单次接收块大小（建议与 provided buffer 一致） |

所有参数均支持配置覆盖。

### 5.4 Receiver 级策略（慢连接治理）

```cpp
if (lag_i <= soft_lag_bytes) {
    state = Healthy;
} else if (lag_i <= hard_lag_bytes) {
    state = Slow;
} else {
    // lag_i > hard_lag_bytes
    // 只有持续 no-progress 超过宽限期才断开
    if (no_progress_ms_i > kick_grace_ms) {
        disconnect(receiver_i);
    }
}
```

### 5.5 Global 级策略（Sender 回压）

```cpp
bool should_pause =
    used_bytes >= capacity * pause_watermark_pct / 100 ||
    free_bytes < recv_chunk_bytes;

bool should_resume =
    used_bytes <= capacity * resume_watermark_pct / 100 &&
    free_bytes >= 2 * recv_chunk_bytes;
```

- 进入 `pause` 后，不再重提 Sender `recv` SQE。
- 进入 `resume` 后，重新提交 Sender `recv`。
- `pause`/`resume` 使用不同阈值，避免频繁抖动切换。

### 5.6 无接收者排空策略（DrainMode）

```cpp
if (receiver_count == 0) {
    // 继续从 Sender 读取，但不进入 RingBuffer
    bytes_dropped_no_receiver += recv_len;
    rearm_sender_recv();
    return;
}
```

用途：
- 保持 Sender 可持续发送，不因无 Receiver 被阻塞。
- 明确语义：无 Receiver 时的数据不会缓存或回放给后续 Receiver。

### 5.7 环形缓冲区大小考量

```
缓冲区建议：256MB（可配置）

计算依据：
- 目标吞吐量：40 Gbps ≈ 5 GB/s
- 允许吸收抖动窗口：~50ms
- 所需缓冲区：5 GB/s × 0.05s = 256 MB
```

## 6. 内存布局

### 6.1 整体内存分配

```
┌─────────────────────────────────────────────────────────────────┐
│                     内存布局 (~300MB)                            │
├─────────────────────────────────────────────────────────────────┤
│  Ring Buffer                                         256 MB     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 用于存储待广播的数据                                       │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  Provided Buffer Pool                                4 MB       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 64 × 64KB buffers for io_uring recv                      │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  Connection State                                    ~1 KB      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 16 receivers × 64 bytes each                             │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  io_uring SQE/CQE rings                              ~1 MB      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 4096 entries × 64 bytes + CQE ring                       │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 6.2 连接状态结构

```cpp
struct ReceiverState {
    int fd;                    // 4 bytes
    uint32_t flags;            // 4 bytes
    size_t read_position;      // 8 bytes
    size_t pending_bytes;      // 8 bytes
    uint64_t last_progress_ns; // 8 bytes
    // 填充到 64 字节以避免 false sharing
    uint8_t padding[32];
};

static_assert(sizeof(ReceiverState) == 64);
```

## 7. 性能优化策略

### 7.1 减少系统调用

| 优化 | 方法 |
|------|------|
| Multishot Accept | 一次提交，多次触发 |
| Batch Submit | 累积多个 SQE 后一次提交 |
| CQE Skip | 成功操作跳过 CQE 生成 |

### 7.2 减少内存拷贝

| 优化 | 方法 |
|------|------|
| Provided Buffers | 预注册接收缓冲区 |
| Zero-Copy Send | 使用 SEND_ZC |
| Ring Buffer 直接读取 | 无需拷贝到发送缓冲区 |

### 7.3 CPU 缓存友好

- 连接状态 64 字节对齐
- 环形缓冲区连续内存
- 热点数据集中存放

### 7.4 内核参数调优

```bash
# 网络缓冲区
sysctl -w net.core.rmem_max=268435456
sysctl -w net.core.wmem_max=268435456
sysctl -w net.ipv4.tcp_rmem="4096 87380 268435456"
sysctl -w net.ipv4.tcp_wmem="4096 87380 268435456"

# TCP 优化
sysctl -w net.ipv4.tcp_congestion_control=bbr
sysctl -w net.core.netdev_max_backlog=250000
```

## 8. 错误处理

### 8.1 错误类型

```cpp
enum class ErrorType {
    // 可恢复错误
    WouldBlock,          // EAGAIN - 重试
    Interrupted,         // EINTR - 重试

    // 连接错误（断开连接）
    ConnectionReset,     // 对端重置
    ConnectionClosed,    // 对端关闭
    BrokenPipe,          // 管道断开

    // 系统错误（记录并继续）
    NoBuffers,           // 缓冲区不足
    ResourceLimit,       // 资源限制

    // 致命错误（退出程序）
    RingSetupFailed,     // io_uring 初始化失败
    SocketFailed,        // socket 创建失败
};
```

### 8.2 错误处理策略

```cpp
void handle_error(int fd, int error_code, OpType op_type) {
    switch (classify_error(error_code)) {
        case ErrorType::WouldBlock:
        case ErrorType::Interrupted:
            // 重新提交操作
            resubmit_operation(fd, op_type);
            break;

        case ErrorType::ConnectionReset:
        case ErrorType::ConnectionClosed:
        case ErrorType::BrokenPipe:
            // 清理连接
            close_connection(fd);
            break;

        case ErrorType::NoBuffers:
            // 等待缓冲区释放
            defer_operation(fd, op_type);
            break;

        default:
            log_error(fd, error_code, op_type);
            break;
    }
}
```

## 9. 监控与指标

### 9.1 关键指标

```cpp
struct Metrics {
    // 吞吐量
    std::atomic<uint64_t> bytes_received;
    std::atomic<uint64_t> bytes_sent;
    std::atomic<uint64_t> bytes_dropped_no_receiver;

    // 连接
    std::atomic<uint32_t> active_receivers;
    std::atomic<uint64_t> total_connections;
    std::atomic<uint64_t> disconnections_lag;

    // 性能
    std::atomic<uint64_t> sqe_submitted;
    std::atomic<uint64_t> cqe_processed;

    // 缓冲区
    std::atomic<size_t> ring_buffer_used;
    std::atomic<size_t> max_receiver_lag;
    std::atomic<uint64_t> sender_recv_pause_count;
    std::atomic<uint64_t> sender_recv_pause_ns;
};
```

### 9.2 实时监控

```cpp
void print_stats(const Metrics& m, double elapsed_sec) {
    double recv_gbps = (m.bytes_received * 8.0) / (elapsed_sec * 1e9);
    double send_gbps = (m.bytes_sent * 8.0) / (elapsed_sec * 1e9);

    std::cout << "Throughput: recv=" << recv_gbps << " Gbps, "
              << "send=" << send_gbps << " Gbps\n"
              << "Receivers: " << m.active_receivers << "\n"
              << "Buffer usage: " << (m.ring_buffer_used / 1024 / 1024) << " MB\n";
}
```

## 10. 扩展考量

### 10.1 未来可能的扩展

1. **多发送者支持**: 允许多个发送者，合并数据流
2. **消息分帧**: 在客户端/网关侧使用 COBS 协议支持消息边界
3. **过滤/路由**: 基于内容的选择性广播
4. **持久化**: 将数据流写入磁盘供回放

### 10.2 多线程扩展 (如果需要)

```
未来可能的多核扩展：
- 一个 io_uring 实例绑定一个核心
- 使用 SO_REUSEPORT 分散连接
- 每核独立的 Ring Buffer
- 核间通信使用 SPSC 队列
```

## 11. 技术约束

### 11.1 内核要求

| 特性 | 最低内核版本 |
|------|-------------|
| io_uring 基础 | 5.1 |
| Multishot Accept | 5.19 |
| Provided Buffers | 5.7 |
| SEND_ZC | 6.0 |
| Buffer Ring | 5.19 |

**推荐内核版本**: 6.1+ (LTS)

### 11.2 硬件要求

- 40Gbps 网卡 (如 Mellanox ConnectX-5/6)
- 足够的 PCIe 带宽
- 至少 1GB 可用内存
- 支持 NUMA 的系统（可选，用于优化）
