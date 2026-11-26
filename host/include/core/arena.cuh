#ifndef ARENA_CUH
#define ARENA_CUH

#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/* ─── CUDA Error Checking Macro ─── */
#define EXM_CUDA_CHECK(call) \
    do { \
        cudaError_t err = (call); \
        if (err != cudaSuccess) { \
            fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
            std::abort(); \
        } \
    } while (0)

/* ─── Flat Arena Allocator ─── */
struct Arena {
    uint8_t* base_ptr;
    size_t   capacity;
    size_t   offset;
};

/* Initialize arena from pre-allocated device memory */
static inline void arena_init(Arena* arena, uint8_t* base_ptr, size_t capacity) {
    arena->base_ptr = base_ptr;
    arena->capacity = capacity;
    arena->offset   = 0;
}

/* Allocate from arena with 256-byte alignment */
static inline void* arena_alloc(Arena* arena, size_t size) {
    size_t aligned_offset = (arena->offset + 255) & ~(size_t)255;
    size_t new_offset = aligned_offset + size;
    if (new_offset > arena->capacity) {
        fprintf(stderr, "Arena OOM: requested %zu bytes, capacity %zu, offset %zu\n",
                size, arena->capacity, arena->offset);
        std::abort();
    }
    void* ptr = arena->base_ptr + aligned_offset;
    arena->offset = new_offset;
    return ptr;
}

/* Reset arena (O(1) - just reset offset) */
static inline void arena_reset(Arena* arena) {
    arena->offset = 0;
}

/* Free arena (no-op for flat allocator, kept for API symmetry) */
static inline void arena_free(Arena* arena) {
    (void)arena;
}

/* ─── CUDA Timer for Benchmarking ─── */
struct CudaTimer {
    cudaEvent_t start;
    cudaEvent_t stop;
    float       elapsed_ms;
};

static inline void cuda_timer_init(CudaTimer* timer) {
    EXM_CUDA_CHECK(cudaEventCreate(&timer->start));
    EXM_CUDA_CHECK(cudaEventCreate(&timer->stop));
    timer->elapsed_ms = 0.0f;
}

static inline void cuda_timer_start(CudaTimer* timer) {
    EXM_CUDA_CHECK(cudaEventRecord(timer->start));
}

static inline void cuda_timer_stop(CudaTimer* timer) {
    EXM_CUDA_CHECK(cudaEventRecord(timer->stop));
    EXM_CUDA_CHECK(cudaEventSynchronize(timer->stop));
    EXM_CUDA_CHECK(cudaEventElapsedTime(&timer->elapsed_ms, timer->start, timer->stop));
}

static inline float cuda_timer_elapsed_ms(const CudaTimer* timer) {
    return timer->elapsed_ms;
}

static inline void cuda_timer_destroy(CudaTimer* timer) {
    EXM_CUDA_CHECK(cudaEventDestroy(timer->start));
    EXM_CUDA_CHECK(cudaEventDestroy(timer->stop));
}

#endif /* ARENA_CUH */