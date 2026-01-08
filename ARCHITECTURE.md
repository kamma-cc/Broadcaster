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
- **原样转发**: 发送者的 TCP 流原封不动地广播给所有接收者
- **动态加入**: 接收者可随时加入/退出，只接收加入后的数据
- **落后断开**: 当接收者处理速度落后时，主动断开连接

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
    // 处理新连接：如果没有发送者，设为发送者；否则设为接收者
    ConnectionRole assign_role(int fd);

    // 检查发送者是否存在
    bool has_sender() const;

    // 发送者断开时调用
    void on_sender_disconnect();
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
    size_t max_lag_bytes_;           // 最大允许落后字节数

public:
    // 检查接收者是否落后
    bool should_disconnect(size_t receiver_pos, size_t write_pos) const;

    // 计算落后程度
    size_t get_lag(size_t receiver_pos, size_t write_pos) const;
};
```

### 2.3 连接角色识别

由于需要区分发送者和接收者，采用以下策略：

**方案: 首连接为发送者**

```cpp
enum class ConnectionRole {
    Sender,     // 发送者：数据源
    Receiver    // 接收者：数据消费者
};

// 第一个连接成为发送者，后续连接成为接收者
// 发送者断开后，下一个新连接成为发送者
```

## 3. io_uring 高级特性应用

### 3.1 Multishot Accept

使用多次触发的 accept 操作，避免每次接受连接后重新提交：

```cpp
void submit_multishot_accept(IoUring& ring, int listen_fd) {
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &accept_op);
}
```

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

### 4.1 接收流程 (从发送者)

```
发送者 TCP 数据
       │
       ▼
┌─────────────────┐
│  io_uring recv  │◄── Provided Buffer
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Ring Buffer   │──► 写入位置更新
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 触发所有接收者  │
│   的发送操作    │
└─────────────────┘
```

### 4.2 发送流程 (到接收者)

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

### 4.3 完整事件循环

```cpp
void event_loop(IoUring& ring, int listen_fd) {
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
    }
}
```

## 5. 背压与落后处理

### 5.1 落后检测策略

```cpp
constexpr size_t MAX_LAG_BYTES = 64 * 1024 * 1024;  // 64MB

bool check_receiver_lag(const ReceiverState& receiver,
                        size_t write_pos) {
    size_t lag = write_pos - receiver.read_position;

    if (lag > MAX_LAG_BYTES) {
        // 接收者落后太多，需要断开
        return true;
    }
    return false;
}
```

### 5.2 断开落后接收者

```cpp
void disconnect_lagging_receiver(IoUring& ring, int fd) {
    // 提交异步 close 操作
    io_uring_sqe* sqe = ring.get_sqe();
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, make_close_op(fd));

    // 从接收者列表中移除
    receiver_manager.remove_receiver(fd);
}
```

### 5.3 环形缓冲区大小考量

```
缓冲区大小建议：256MB

计算依据：
- 目标吞吐量：40 Gbps ≈ 5 GB/s
- 允许最大延迟：~50ms
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
    // 填充到 64 字节以避免 false sharing
    uint8_t padding[40];
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
2. **消息分帧**: 使用 COBS 协议支持消息边界
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
