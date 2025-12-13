# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Broadcaster is a high-performance network broadcast service built with io_uring, supporting the Redis Sharded Pub/Sub protocol.

## Current Status

**Phase 1 Complete**: Basic io_uring TCP server implementation
- ✅ io_uring initialization and event loop
- ✅ TCP socket setup and listening
- ✅ Asynchronous accept using IORING_OP_ACCEPT
- ✅ Basic connection handling

**Current Phase**: RAII resource encapsulation
- **Next**: Encapsulate Linux network programming resources (Socket, IoUring) using C++ RAII patterns
- **Future**: Implement multishot operations (accept, recv), provided buffers, zero-copy features

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

**Next Steps**:
1. Create RAII wrappers for socket file descriptors
2. Create RAII wrapper for io_uring ring
3. Implement multishot accept for efficient connection handling
4. Add receive/send operations with buffer management

## Development Environment

The project is intended to be developed on Linux systems (io_uring requires Linux kernel 5.1+).