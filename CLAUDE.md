# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Broadcaster 是一个高性能的 TCP 流广播服务，设计目标：

- **单发送者**: 一个 TCP 连接作为数据源
- **多接收者**: 最多 16 个接收者同时接收数据
- **原样转发**: 发送者的 TCP 流原封不动地广播给所有接收者
- **动态加入**: 接收者可随时加入/退出，只接收加入后的数据
- **落后断开**: 当接收者处理速度落后时，主动断开连接
- **性能目标**: 单线程模式下达到 40Gbps 吞吐量

## Documentation

- **ARCHITECTURE.md**: 详细架构设计文档
- **DEVELOPMENT_ROADMAP.md**: 开发规划与阶段任务
- **PROTOCOL_DESIGN.md**: COBS 协议设计（用于消息边界识别）

## Current Status

**Phase 2 Complete**: RAII resource encapsulation with Hello World server
- ✅ io_uring initialization and event loop
- ✅ TCP socket setup and listening
- ✅ Asynchronous accept using IORING_OP_ACCEPT
- ✅ Asynchronous send using IORING_OP_SEND
- ✅ Asynchronous close using IORING_OP_CLOSE
- ✅ RAII wrappers for FileDescriptor, Socket, IoUring

**Current Implementation** (src/main.cpp):
- `FileDescriptor` class: Generic fd wrapper (non-copyable, movable)
- `Socket` class: TCP socket with `create_listener()` factory method
- `IoUring` class: io_uring ring wrapper (non-copyable, non-movable)
- Simple "Hello, World!" TCP server for testing

**Next Phase** (Phase 3): Core Broadcasting Features
- ConnectionManager: 连接角色分配与管理
- RingBuffer: 256MB 环形缓冲区
- Basic broadcast logic: 从发送者接收，广播到所有接收者

## Implementation Strategy

**Core Technology Stack**:
- **Use native liburing** - Direct io_uring API for maximum control and performance
- **No high-level frameworks** - Avoid Boost.Asio, libuv, or similar abstractions
- **Why liburing**: Full access to advanced features (multishot accept/recv, provided buffers, zero-copy)

**Architecture Principles**:
- 单线程模式，充分利用 io_uring 异步能力
- C++ RAII 管理 Linux 资源 (socket fd, io_uring ring, buffers)
- 零拷贝传输：provided buffers (接收) + send_zc (发送)
- 环形缓冲区存储待广播数据，支持多接收者独立读取位置
- 背压控制：落后接收者自动断开

**Performance Targets**:
- 40 Gbps total throughput (sender + 16 receivers)
- Single-threaded operation
- < 1ms latency (P99)

## Build Commands

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run
./build/broadcaster
```

## Development Environment

- **OS**: Linux (kernel 6.1+ recommended for all io_uring features)
- **Compiler**: Clang with C++23 support
- **Dependencies**: liburing

## Key Design Decisions

1. **Single-threaded**: io_uring can saturate network with single thread
2. **Ring buffer size**: 256MB (allows ~50ms lag at 40Gbps)
3. **First connection becomes sender**: Simple role assignment
4. **Zero-copy everywhere**: provided buffers + send_zc

## File Structure (Planned)

```
src/
├── main.cpp                 # Entry point
├── broadcaster.hpp/cpp      # Main broadcaster class
├── io_uring_wrapper.hpp/cpp # io_uring RAII wrapper
├── socket.hpp/cpp           # Socket RAII wrapper
├── ring_buffer.hpp/cpp      # Ring buffer implementation
├── connection_manager.hpp/cpp # Connection management
├── buffer_ring.hpp/cpp      # Provided buffers
└── backpressure.hpp/cpp     # Backpressure control
```
