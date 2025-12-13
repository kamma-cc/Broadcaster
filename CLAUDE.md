# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Broadcaster is a high-performance network broadcast service built with io_uring, supporting the Redis Sharded Pub/Sub protocol.

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

**Next Phase**: Production features
- Implement multishot accept for efficient connection handling
- Add receive operations (IORING_OP_RECV)
- Buffer management with provided buffers
- Zero-copy optimizations

**Protocol Design**: COBS encoding documented in PROTOCOL_DESIGN.md

## Implementation Strategy

**Core Technology Stack**:
- **Use native liburing** - Direct io_uring API for maximum control and performance
- **No high-level frameworks** - Avoid Boost.Asio, libuv, or similar abstractions
- **Why liburing**: Full access to advanced features (multishot accept/recv, provided buffers, zero-copy)

**Architecture Principles**:
- Use C++ RAII to manage Linux resources (socket fd, io_uring ring, buffers)
- Focus on zero-copy and efficient memory management patterns
- Leverage modern io_uring features for extreme performance
- The service implements Redis Sharded Pub/Sub protocol for compatibility with Redis clients

## Development Environment

The project is intended to be developed on Linux systems (io_uring requires Linux kernel 5.1+).