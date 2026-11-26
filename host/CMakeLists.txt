cmake_minimum_required(VERSION 3.24)
project(exmachina_host LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Architecture: sm_86 for RTX 3050 (Ampere)
set(CMAKE_CUDA_ARCHITECTURES 86)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra")
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3 -lineinfo -Wall -Wextra")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../shared ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Enable CUDA language (modern CMake)
enable_language(CUDA)

# Source files
set(COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/exm_packet.h
)

set(BENCH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main_bench.cu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ast/indexer.cpp
)

# Benchmark executable
add_executable(exm-bench ${BENCH_SOURCES})
# No need to link CUDA libraries explicitly with modern CMake

# Daemon executable
set(EXMD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/exmd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/sandbox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ast/indexer.cpp
)
add_executable(exmd ${EXMD_SOURCES})

# Install targets (optional)
install(TARGETS exm-bench exmd DESTINATION bin)