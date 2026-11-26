#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <random>

#include "../shared/exm_packet.h"
#include "core/arena.cuh"
#include "kernels/rrf_topk.cuh"
#include "ast/indexer.h"

/* ─── Benchmark Configuration ─── */
constexpr size_t ARENA_CAPACITY = 64 * 1024 * 1024;  // 64 MB
constexpr size_t TEST_ALLOC_SIZE = 4 * 1024 * 1024;  // 4 MB
constexpr int    ITERATIONS = 1000;
constexpr int    WARMUP_ITERS = 10;

/* ─── RRF Test Configuration ─── */
constexpr int N_DOCS = 10000;
constexpr int D_DIM = 384;
constexpr int K_TOP = 8;

/* ─── Kernel: 4 MB float write for bandwidth test ─── */
__global__ void bandwidth_write_kernel(float* dst, size_t num_elements, float value) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < num_elements) {
        dst[idx] = value;
    }
}

/* ─── CRC-16 Test Functions ─── */
void test_crc16_roundtrip() {
    printf("Testing CRC-16 round-trip...\n");
    
    ExmProbeIntent intent = {};
    intent.intent_id = INTENT_SCAN_BUS;
    intent.confidence = 200;
    intent.seq_num = 42;
    intent.rail_mv = 3300;
    intent.ripple_mv = 50;
    intent.i2c_count = 3;
    intent.i2c_addrs[0] = 0x48;
    intent.i2c_addrs[1] = 0x68;
    intent.i2c_addrs[2] = 0x77;
    intent.who_am_i = 0xD0;
    // hmac_sha256 left as zeros for this test
    
    // Build full packet: header + payload
    ExmHeader hdr = {};
    hdr.magic = 0x4558;
    hdr.type = PKT_PROBE_INTENT;
    hdr.flags = 0x02;  // HMAC signed
    hdr.payload_len = sizeof(ExmProbeIntent);
    
    // Compute CRC over header (6 bytes: magic..payload_len) + payload
    uint8_t crc_buffer[6 + sizeof(ExmProbeIntent)];
    std::memcpy(crc_buffer, &hdr, 6);  // magic(2) + type(1) + flags(1) + payload_len(2) = 6
    std::memcpy(crc_buffer + 6, &intent, sizeof(ExmProbeIntent));
    
    uint16_t crc = exm_crc16(crc_buffer, sizeof(crc_buffer));
    hdr.crc16 = crc;
    
    // Verify by recomputing
    std::memcpy(crc_buffer, &hdr, 6);
    std::memcpy(crc_buffer + 6, &intent, sizeof(ExmProbeIntent));
    uint16_t crc_verify = exm_crc16(crc_buffer, sizeof(crc_buffer));
    
    if (crc == crc_verify) {
        printf("  CRC-16 round-trip: PASS (0x%04X)\n", crc);
    } else {
        printf("  CRC-16 round-trip: FAIL (got 0x%04X, expected 0x%04X)\n", crc_verify, crc);
        std::abort();
    }
}

void test_crc16_bitflip() {
    printf("Testing CRC-16 bit-flip detection...\n");
    
    ExmProbeIntent intent = {};
    intent.intent_id = INTENT_CHECK_RAIL;
    intent.seq_num = 1;
    intent.rail_mv = 5000;
    intent.ripple_mv = 100;
    
    ExmHeader hdr = {};
    hdr.magic = 0x4558;
    hdr.type = PKT_PROBE_INTENT;
    hdr.flags = 0x02;
    hdr.payload_len = sizeof(ExmProbeIntent);
    
    uint8_t crc_buffer[6 + sizeof(ExmProbeIntent)];
    std::memcpy(crc_buffer, &hdr, 6);
    std::memcpy(crc_buffer + 6, &intent, sizeof(ExmProbeIntent));
    uint16_t crc_orig = exm_crc16(crc_buffer, sizeof(crc_buffer));
    
    // Flip a bit in the payload
    crc_buffer[6 + 4] ^= 0x01;  // Flip bit in seq_num
    uint16_t crc_corrupted = exm_crc16(crc_buffer, sizeof(crc_buffer));
    
    if (crc_orig != crc_corrupted) {
        printf("  CRC-16 bit-flip detection: PASS\n");
    } else {
        printf("  CRC-16 bit-flip detection: FAIL (CRC unchanged after corruption)\n");
        std::abort();
    }
}

/* ─── CPU Reference Implementation for Cosine Similarity ─── */
void cpu_cosine_sim(const float* query, const float* docs, float* scores, int N, int D) {
    for (int n = 0; n < N; ++n) {
        float sum = 0.0f;
        for (int d = 0; d < D; ++d) {
            sum += query[d] * docs[n * D + d];
        }
        scores[n] = sum;
    }
}

/* ─── CPU Reference Implementation for RRF + Top-K ─── */
void cpu_rrf_topk(const float* dense_scores, const float* sparse_scores, int N, int K,
                  int* topk_indices, float* topk_scores) {
    // Compute ranks
    std::vector<int> dense_ranks(N), sparse_ranks(N);
    
    for (int i = 0; i < N; ++i) {
        int dr = 0, sr = 0;
        for (int j = 0; j < N; ++j) {
            if (dense_scores[j] > dense_scores[i]) dr++;
            if (sparse_scores[j] > sparse_scores[i]) sr++;
        }
        dense_ranks[i] = dr;
        sparse_ranks[i] = sr;
    }
    
    // Compute RRF scores
    std::vector<float> rrf_scores(N);
    for (int i = 0; i < N; ++i) {
        rrf_scores[i] = 1.0f / (60.0f + dense_ranks[i]) + 1.0f / (60.0f + sparse_ranks[i]);
    }
    
    // Find top-K
    std::vector<std::pair<float, int>> pairs(N);
    for (int i = 0; i < N; ++i) {
        pairs[i] = {rrf_scores[i], i};
    }
    std::partial_sort(pairs.begin(), pairs.begin() + K, pairs.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    
    for (int k = 0; k < K; ++k) {
        topk_scores[k] = pairs[k].first;
        topk_indices[k] = pairs[k].second;
    }
}

/* ─── Test Cosine Similarity Kernel ─── */
void test_cosine_sim_kernel() {
    printf("\nTesting Cosine Similarity Kernel (N=%d, D=%d)...\n", N_DOCS, D_DIM);
    
    // Generate random normalized vectors
    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    
    std::vector<float> h_query(D_DIM);
    std::vector<float> h_docs(N_DOCS * D_DIM);
    
    for (int i = 0; i < D_DIM; ++i) h_query[i] = dist(rng);
    for (int i = 0; i < N_DOCS * D_DIM; ++i) h_docs[i] = dist(rng);
    
    // Normalize query
    float q_norm = 0.0f;
    for (int i = 0; i < D_DIM; ++i) q_norm += h_query[i] * h_query[i];
    q_norm = sqrtf(q_norm);
    for (int i = 0; i < D_DIM; ++i) h_query[i] /= q_norm;
    
    // Normalize docs
    for (int n = 0; n < N_DOCS; ++n) {
        float d_norm = 0.0f;
        for (int d = 0; d < D_DIM; ++d) {
            float v = h_docs[n * D_DIM + d];
            d_norm += v * v;
        }
        d_norm = sqrtf(d_norm);
        for (int d = 0; d < D_DIM; ++d) {
            h_docs[n * D_DIM + d] /= d_norm;
        }
    }
    
    // CPU reference
    std::vector<float> h_cpu_scores(N_DOCS);
    cpu_cosine_sim(h_query.data(), h_docs.data(), h_cpu_scores.data(), N_DOCS, D_DIM);
    
    // Allocate device memory
    float *d_query, *d_docs, *d_scores;
    EXM_CUDA_CHECK(cudaMalloc(&d_query, D_DIM * sizeof(float)));
    EXM_CUDA_CHECK(cudaMalloc(&d_docs, N_DOCS * D_DIM * sizeof(float)));
    EXM_CUDA_CHECK(cudaMalloc(&d_scores, N_DOCS * sizeof(float)));
    
    EXM_CUDA_CHECK(cudaMemcpy(d_query, h_query.data(), D_DIM * sizeof(float), cudaMemcpyHostToDevice));
    EXM_CUDA_CHECK(cudaMemcpy(d_docs, h_docs.data(), N_DOCS * D_DIM * sizeof(float), cudaMemcpyHostToDevice));
    
    // Create arena for timing
    uint8_t* d_arena_base;
    EXM_CUDA_CHECK(cudaMalloc(&d_arena_base, ARENA_CAPACITY));
    Arena arena;
    arena_init(&arena, d_arena_base, ARENA_CAPACITY);
    
    CudaTimer timer;
    cuda_timer_init(&timer);
    
    // Warmup
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        launch_cosine_sim(d_query, d_docs, d_scores, N_DOCS, D_DIM, &arena);
        EXM_CUDA_CHECK(cudaDeviceSynchronize());
    }
    
    // Benchmark
    cuda_timer_start(&timer);
    for (int i = 0; i < 100; ++i) {
        launch_cosine_sim(d_query, d_docs, d_scores, N_DOCS, D_DIM, &arena);
    }
    cuda_timer_stop(&timer);
    float gpu_time_ms = timer.elapsed_ms / 100.0f;
    
    // Copy results back
    std::vector<float> h_gpu_scores(N_DOCS);
    EXM_CUDA_CHECK(cudaMemcpy(h_gpu_scores.data(), d_scores, N_DOCS * sizeof(float), cudaMemcpyDeviceToHost));
    
    // Verify accuracy
    float max_abs_err = 0.0f;
    for (int i = 0; i < N_DOCS; ++i) {
        float err = fabsf(h_gpu_scores[i] - h_cpu_scores[i]);
        if (err > max_abs_err) max_abs_err = err;
    }
    
    printf("  GPU time: %.4f ms\n", gpu_time_ms);
    printf("  Max abs error vs CPU: %.6f\n", max_abs_err);
    
    if (max_abs_err < 1e-5f) {
        printf("  Accuracy test: PASS (max_abs_err < 1e-5)\n");
    } else {
        printf("  Accuracy test: FAIL (max_abs_err = %.6f)\n", max_abs_err);
        std::abort();
    }
    
    if (gpu_time_ms < 1.0f) {
        printf("  Latency test: PASS (< 1.0 ms)\n");
    } else {
        printf("  Latency test: FAIL (%.4f ms >= 1.0 ms)\n", gpu_time_ms);
        std::abort();
    }
    
    cuda_timer_destroy(&timer);
    EXM_CUDA_CHECK(cudaFree(d_query));
    EXM_CUDA_CHECK(cudaFree(d_docs));
    EXM_CUDA_CHECK(cudaFree(d_scores));
    EXM_CUDA_CHECK(cudaFree(d_arena_base));
}

/* ─── Test RRF + Top-K Kernel ─── */
void test_rrf_topk_kernel() {
    printf("\nTesting RRF + Top-K Kernel (N=%d, K=%d)...\n", N_DOCS, K_TOP);
    fflush(stdout);
    
    // Generate test scores
    std::mt19937 rng(123);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    
    std::vector<float> h_dense(N_DOCS), h_sparse(N_DOCS);
    for (int i = 0; i < N_DOCS; ++i) {
        h_dense[i] = dist(rng);
        h_sparse[i] = dist(rng);
    }
    
    // CPU reference
    std::vector<int> h_cpu_topk_idx(K_TOP);
    std::vector<float> h_cpu_topk_scores(K_TOP);
    cpu_rrf_topk(h_dense.data(), h_sparse.data(), N_DOCS, K_TOP,
                 h_cpu_topk_idx.data(), h_cpu_topk_scores.data());
    printf("  CPU reference computed\n");
    fflush(stdout);
    
    // Allocate device memory
    float *d_dense, *d_sparse;
    int *d_topk_idx;
    float *d_topk_scores;
    EXM_CUDA_CHECK(cudaMalloc(&d_dense, N_DOCS * sizeof(float)));
    EXM_CUDA_CHECK(cudaMalloc(&d_sparse, N_DOCS * sizeof(float)));
    EXM_CUDA_CHECK(cudaMalloc(&d_topk_idx, K_TOP * sizeof(int)));
    EXM_CUDA_CHECK(cudaMalloc(&d_topk_scores, K_TOP * sizeof(float)));
    printf("  Device memory allocated\n");
    fflush(stdout);
    
    EXM_CUDA_CHECK(cudaMemcpy(d_dense, h_dense.data(), N_DOCS * sizeof(float), cudaMemcpyHostToDevice));
    EXM_CUDA_CHECK(cudaMemcpy(d_sparse, h_sparse.data(), N_DOCS * sizeof(float), cudaMemcpyHostToDevice));
    printf("  Data copied to device\n");
    fflush(stdout);
    
    // Create arena for timing
    uint8_t* d_arena_base;
    EXM_CUDA_CHECK(cudaMalloc(&d_arena_base, ARENA_CAPACITY));
    Arena arena;
    arena_init(&arena, d_arena_base, ARENA_CAPACITY);
    printf("  Arena initialized\n");
    fflush(stdout);
    
    CudaTimer timer;
    cuda_timer_init(&timer);
    
    // Warmup
    printf("  Starting warmup...\n");
    fflush(stdout);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        launch_rrf_topk(d_dense, d_sparse, N_DOCS, K_TOP, d_topk_idx, d_topk_scores, &arena);
        EXM_CUDA_CHECK(cudaDeviceSynchronize());
    }
    printf("  Warmup done\n");
    fflush(stdout);
    
    // Benchmark
    printf("  Starting benchmark...\n");
    fflush(stdout);
    cuda_timer_start(&timer);
    for (int i = 0; i < 100; ++i) {
        launch_rrf_topk(d_dense, d_sparse, N_DOCS, K_TOP, d_topk_idx, d_topk_scores, &arena);
    }
    cuda_timer_stop(&timer);
    float gpu_time_ms = timer.elapsed_ms / 100.0f;
    printf("  Benchmark done\n");
    fflush(stdout);
    
    // Copy results back
    std::vector<int> h_gpu_topk_idx(K_TOP);
    std::vector<float> h_gpu_topk_scores(K_TOP);
    EXM_CUDA_CHECK(cudaMemcpy(h_gpu_topk_idx.data(), d_topk_idx, K_TOP * sizeof(int), cudaMemcpyDeviceToHost));
    EXM_CUDA_CHECK(cudaMemcpy(h_gpu_topk_scores.data(), d_topk_scores, K_TOP * sizeof(float), cudaMemcpyDeviceToHost));
    printf("  Results copied back\n");
    fflush(stdout);
    
    // Verify Top-K match
    printf("  GPU topk_idx: ");
    for (int k = 0; k < K_TOP; ++k) printf("%d ", h_gpu_topk_idx[k]);
    printf("\n");
    printf("  CPU topk_idx: ");
    for (int k = 0; k < K_TOP; ++k) printf("%d ", h_cpu_topk_idx[k]);
    printf("\n");
    printf("  GPU topk_scores: ");
    for (int k = 0; k < K_TOP; ++k) printf("%.6f ", h_gpu_topk_scores[k]);
    printf("\n");
    printf("  CPU topk_scores: ");
    for (int k = 0; k < K_TOP; ++k) printf("%.6f ", h_cpu_topk_scores[k]);
    printf("\n");
    fflush(stdout);
    
    bool match = true;
    for (int k = 0; k < K_TOP; ++k) {
        if (h_gpu_topk_idx[k] != h_cpu_topk_idx[k] || 
            fabsf(h_gpu_topk_scores[k] - h_cpu_topk_scores[k]) > 1e-4f) {
            match = false;
            printf("  Mismatch at k=%d: GPU(idx=%d, score=%.6f) vs CPU(idx=%d, score=%.6f)\n",
                   k, h_gpu_topk_idx[k], h_gpu_topk_scores[k],
                   h_cpu_topk_idx[k], h_cpu_topk_scores[k]);
        }
    }
    
    printf("  GPU time: %.4f ms\n", gpu_time_ms);
    fflush(stdout);
    
    if (match) {
        printf("  Top-K match test: PASS\n");
    } else {
        printf("  Top-K match test: FAIL\n");
        std::abort();
    }
    
    // Note: O(N^2) rank computation is bottleneck; < 5ms is realistic for N=10000 on RTX 3050
    if (gpu_time_ms < 5.0f) {
        printf("  Latency test: PASS (< 5.0 ms)\n");
    } else {
        printf("  Latency test: FAIL (%.4f ms >= 5.0 ms)\n", gpu_time_ms);
        std::abort();
    }
    
    cuda_timer_destroy(&timer);
    EXM_CUDA_CHECK(cudaFree(d_dense));
    EXM_CUDA_CHECK(cudaFree(d_sparse));
    EXM_CUDA_CHECK(cudaFree(d_topk_idx));
    EXM_CUDA_CHECK(cudaFree(d_topk_scores));
    EXM_CUDA_CHECK(cudaFree(d_arena_base));
}

/* ─── Arena vs cudaMalloc Benchmark ─── */
void benchmark_arena_vs_cudamalloc() {
    printf("\nBenchmarking Arena vs cudaMalloc/cudaFree (%d iterations, %zu MB each)...\n",
           ITERATIONS, TEST_ALLOC_SIZE / (1024*1024));
    
    // Pre-allocate arena
    uint8_t* d_arena_base;
    EXM_CUDA_CHECK(cudaMalloc(&d_arena_base, ARENA_CAPACITY));
    Arena arena;
    arena_init(&arena, d_arena_base, ARENA_CAPACITY);
    
    CudaTimer timer;
    cuda_timer_init(&timer);
    
    // Warmup
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        void* ptr = arena_alloc(&arena, TEST_ALLOC_SIZE);
        EXM_CUDA_CHECK(cudaMemset(ptr, 0, TEST_ALLOC_SIZE));
        arena_reset(&arena);
    }
    EXM_CUDA_CHECK(cudaDeviceSynchronize());
    
    // Benchmark Arena
    cuda_timer_start(&timer);
    for (int i = 0; i < ITERATIONS; ++i) {
        void* ptr = arena_alloc(&arena, TEST_ALLOC_SIZE);
        EXM_CUDA_CHECK(cudaMemset(ptr, 0, TEST_ALLOC_SIZE));
        arena_reset(&arena);
    }
    cuda_timer_stop(&timer);
    float arena_time_ms = timer.elapsed_ms;
    
    // Warmup cudaMalloc
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        void* ptr;
        EXM_CUDA_CHECK(cudaMalloc(&ptr, TEST_ALLOC_SIZE));
        EXM_CUDA_CHECK(cudaMemset(ptr, 0, TEST_ALLOC_SIZE));
        EXM_CUDA_CHECK(cudaFree(ptr));
    }
    EXM_CUDA_CHECK(cudaDeviceSynchronize());
    
    // Benchmark cudaMalloc/cudaFree
    cuda_timer_start(&timer);
    for (int i = 0; i < ITERATIONS; ++i) {
        void* ptr;
        EXM_CUDA_CHECK(cudaMalloc(&ptr, TEST_ALLOC_SIZE));
        EXM_CUDA_CHECK(cudaMemset(ptr, 0, TEST_ALLOC_SIZE));
        EXM_CUDA_CHECK(cudaFree(ptr));
    }
    cuda_timer_stop(&timer);
    float malloc_time_ms = timer.elapsed_ms;
    
    cuda_timer_destroy(&timer);
    EXM_CUDA_CHECK(cudaFree(d_arena_base));
    
    float speedup = malloc_time_ms / arena_time_ms;
    printf("  cudaMalloc/Free: %.2f ms total (%.4f ms/iter)\n", malloc_time_ms, malloc_time_ms / ITERATIONS);
    printf("  Arena alloc/reset: %.2f ms total (%.4f ms/iter)\n", arena_time_ms, arena_time_ms / ITERATIONS);
    printf("  Speedup: %.1fx\n", speedup);
    
    if (speedup > 5.0f) {
        printf("  Arena speedup > 5x: PASS\n");
    } else {
        printf("  Arena speedup > 5x: FAIL (got %.1fx)\n", speedup);
        std::abort();
    }
}

/* ─── Bandwidth Test ─── */
void benchmark_bandwidth() {
    printf("\nBenchmarking device memory bandwidth (4 MB float write)...\n");
    
    constexpr size_t num_elements = TEST_ALLOC_SIZE / sizeof(float);
    float* d_buffer;
    EXM_CUDA_CHECK(cudaMalloc(&d_buffer, TEST_ALLOC_SIZE));
    
    CudaTimer timer;
    cuda_timer_init(&timer);
    
    // Warmup
    int block_size = 256;
    int grid_size = (num_elements + block_size - 1) / block_size;
    bandwidth_write_kernel<<<grid_size, block_size>>>(d_buffer, num_elements, 1.0f);
    EXM_CUDA_CHECK(cudaDeviceSynchronize());
    
    // Benchmark
    cuda_timer_start(&timer);
    for (int i = 0; i < 100; ++i) {
        bandwidth_write_kernel<<<grid_size, block_size>>>(d_buffer, num_elements, (float)i);
    }
    cuda_timer_stop(&timer);
    
    float elapsed_ms = timer.elapsed_ms;
    float gb_written = (num_elements * sizeof(float) * 100) / (1024.0f * 1024.0f * 1024.0f);
    float gb_per_sec = gb_written / (elapsed_ms / 1000.0f);
    
    printf("  Time: %.2f ms for %.2f GB\n", elapsed_ms, gb_written);
    printf("  Bandwidth: %.2f GB/s\n", gb_per_sec);
    
    cuda_timer_destroy(&timer);
    EXM_CUDA_CHECK(cudaFree(d_buffer));
}

/* ─── Query GPU Properties ─── */
void query_gpu_properties() {
    printf("Querying GPU properties...\n");
    cudaDeviceProp prop;
    EXM_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    printf("  Device: %s\n", prop.name);
    printf("  Compute Capability: %d.%d\n", prop.major, prop.minor);
    printf("  Total Global Memory: %.2f MB\n", prop.totalGlobalMem / (1024.0f * 1024.0f));
    printf("  Multiprocessors: %d\n", prop.multiProcessorCount);
#if defined(CUDA_VERSION) && CUDA_VERSION < 12000
    printf("  Clock Rate: %.2f GHz\n", prop.clockRate / 1e6f);
    printf("  Memory Clock Rate: %.2f GHz\n", prop.memoryClockRate / 1e6f);
#endif
    printf("  Memory Bus Width: %d bits\n", prop.memoryBusWidth);
}

/* ─── Test Indexer ─── */
void test_indexer() {
    printf("\nTesting Structural Indexer (M4)...\n");
    
    ExmIndexer idx;
    ExmIndexerConfig config;
    config.max_chunk_tokens = 512;
    config.min_chunk_tokens = 32;
    config.bm25_k1 = 1.5f;
    config.bm25_b = 0.75f;
    config.embedding_dim = 384;
    exm_indexer_init(&idx, &config);
    
    // Index the shared/exm_packet.h file
    const char* test_file = "/home/tejas/Projects/exmachina/shared/exm_packet.h";
    int result = exm_indexer_add_file(&idx, test_file);
    if (result != 0) {
        printf("  Failed to index %s\n", test_file);
        std::abort();
    }
    
    int num_chunks = exm_indexer_chunk_count(&idx);
    if (num_chunks == 0) {
        printf("  No chunks generated\n");
        std::abort();
    }
    
    // Build BM25 statistics
    exm_indexer_build_bm25(&idx);
    
    // Test query: "ExmProbeIntent hmac_sha256"
    const char* query = "ExmProbeIntent hmac_sha256";
    
    // Compute BM25 scores
    std::vector<float> bm25_scores(num_chunks);
    exm_indexer_bm25_scores(&idx, query, bm25_scores.data());
    
    // Compute query embedding
    std::vector<float> query_embedding(config.embedding_dim);
    exm_indexer_embed_query(&idx, query, query_embedding.data());
    
    // Find chunk with highest BM25 score
    int best_idx = 0;
    float best_score = bm25_scores[0];
    for (int i = 1; i < num_chunks; ++i) {
        if (bm25_scores[i] > best_score) {
            best_score = bm25_scores[i];
            best_idx = i;
        }
    }
    
    const ExmChunk* best_chunk = exm_indexer_get_chunk(&idx, best_idx);
    
    // Verify the best chunk contains the query terms
    std::string chunk_text(best_chunk->text);
    bool has_intent = chunk_text.find("ExmProbeIntent") != std::string::npos;
    bool has_hmac = chunk_text.find("hmac_sha256") != std::string::npos;
    
    if (!has_intent || !has_hmac) {
        printf("  Query terms not found in best chunk\n");
        std::abort();
    }
    
    // Verify it's the ExmProbeIntent struct chunk
    if (!best_chunk->symbol || std::string(best_chunk->symbol).find("ExmProbeIntent") == std::string::npos) {
        printf("  Best chunk is not ExmProbeIntent struct\n");
        std::abort();
    }
    
    exm_indexer_free(&idx);
    printf("  Indexer test: PASS\n");
}

/* ─── Main ─── */
int main() {
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  ExMachina M1+M3+M4 Benchmark: Protocol, Arena, RRF/TopK, Indexer\n");
    printf("═══════════════════════════════════════════════════════════\n\n");
    
    query_gpu_properties();
    
    test_crc16_roundtrip();
    test_crc16_bitflip();
    
    benchmark_arena_vs_cudamalloc();
    benchmark_bandwidth();
    
    test_cosine_sim_kernel();
    test_rrf_topk_kernel();
    
    test_indexer();
    
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  All M1+M3+M4 tests PASSED\n");
    printf("═══════════════════════════════════════════════════════════\n");
    
    return 0;
}