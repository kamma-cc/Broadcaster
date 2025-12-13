# Broadcaster
A high-performance network broadcast service built with io_uring

## Requirements

- Linux kernel 5.1+ (for io_uring support)
- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 11+)
- liburing-dev

### Installing Dependencies

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y liburing-dev cmake build-essential
```

## Building

```bash
mkdir build
cd build
cmake ..
make
```

## Running

```bash
./broadcaster
```
