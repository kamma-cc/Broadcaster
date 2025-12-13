cmake_minimum_required(VERSION 3.20)

# Use clang compiler
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(Broadcaster VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(broadcaster
    src/main.cpp
)

# liburing will be linked when RAII wrappers are implemented
# target_link_libraries(broadcaster PRIVATE uring)