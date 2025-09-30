# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Broadcaster is a high-performance network broadcast service built with io_uring, supporting the Redis Sharded Pub/Sub protocol.

## Current Status

This is a new repository with minimal structure. The project is in the initial planning/setup phase.

## Architecture Notes

When implementing this project:
- The core will use io_uring for high-performance async I/O operations on Linux
- The service implements Redis Sharded Pub/Sub protocol for compatibility with Redis clients
- Focus on zero-copy and efficient memory management patterns typical of io_uring applications

## Development Environment

The project is intended to be developed on Linux systems (io_uring requires Linux kernel 5.1+).