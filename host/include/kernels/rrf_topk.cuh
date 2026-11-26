#ifndef RRF_TOPK_CUH
#define RRF_TOPK_CUH

#include <cuda_runtime.h>
#include <cstdint>
#include <cmath>

#include "core/arena.cuh"

/* ─── Cosine Similarity Kernel ─── */
/* Computes dot product of normalized query vector against N normalized document vectors
 * Using warp-shuffle reduction for efficiency */
__global__ void cosine_sim_kernel(
    const float* __restrict__ query,      // [D] - normalized query vector (D=384)
    const float* __restrict__ docs,       // [N, D] - normalized document vectors
    float* __restrict__ scores,           // [N] - output cosine similarity scores
    int N, int D
) {
    extern __shared__ float smem[];
    float* warp_sums = smem;
    
    int tid = threadIdx.x;
    int bid = blockIdx.x;
    int warp_id = tid / 32;
    int lane_id = tid % 32;
    int warps_per_block = blockDim.x / 32;
    
    int doc_idx = bid * warps_per_block + warp_id;
    if (doc_idx >= N) return;
    
    // Each warp computes dot product for one document
    float sum = 0.0f;
    for (int i = lane_id; i < D; i += 32) {
        sum += query[i] * docs[doc_idx * D + i];
    }
    
    // Warp-level reduction using shuffle
    for (int offset = 16; offset > 0; offset >>= 1) {
        sum += __shfl_down_sync(0xFFFFFFFF, sum, offset);
    }
    
    // First lane in warp writes result
    if (lane_id == 0) {
        warp_sums[warp_id] = sum;
    }
    __syncthreads();
    
    if (lane_id == 0) {
        scores[doc_idx] = warp_sums[warp_id];
    }
}

/* ─── Rank Computation Kernel (Shared Memory Optimized) ─── */
/* Loads all scores into shared memory, then each thread computes rank from shared memory
 * Requires N <= 10000 (40KB shared memory) */
__global__ void compute_ranks_kernel(
    const float* __restrict__ scores, int N, int* __restrict__ ranks
) {
    extern __shared__ float s_scores[];
    int tid = threadIdx.x;
    int block_size = blockDim.x;
    
    // Load all scores into shared memory (coalesced)
    for (int i = tid; i < N; i += block_size) {
        s_scores[i] = scores[i];
    }
    __syncthreads();
    
    // Each thread computes rank for its assigned elements
    for (int idx = tid; idx < N; idx += block_size) {
        float my_score = s_scores[idx];
        int rank = 0;
        // Compare against all scores in shared memory
        for (int i = 0; i < N; ++i) {
            if (s_scores[i] > my_score) rank++;
        }
        ranks[idx] = rank;
    }
}

/* ─── RRF Score Computation Kernel ─── */
/* Computes RRF score for each document: RRF = 1/(60+rank_dense) + 1/(60+rank_sparse) */
__global__ void compute_rrf_kernel(
    const int* __restrict__ dense_ranks,
    const int* __restrict__ sparse_ranks,
    int N,
    float* __restrict__ rrf_scores
) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= N) return;
    
    float rrf = 1.0f / (60.0f + dense_ranks[tid]) + 1.0f / (60.0f + sparse_ranks[tid]);
    rrf_scores[tid] = rrf;
}

/* ─── Top-K Selection Kernel ─── */
/* Finds top-K indices and scores from array using single-block reduction */
__global__ void topk_kernel(
    const float* __restrict__ scores,
    int N, int K,
    int* __restrict__ topk_indices,
    float* __restrict__ topk_scores
) {
    extern __shared__ char topk_smem[];
    float* s_scores = (float*)topk_smem;
    int* s_indices = (int*)(topk_smem + blockDim.x * K * sizeof(float));
    
    int tid = threadIdx.x;
    int block_size = blockDim.x;
    
    // Each thread loads multiple elements if N > block_size
    // Initialize local top-K
    float local_scores[8];  // K <= 8
    int local_indices[8];
    for (int k = 0; k < K; ++k) {
        local_scores[k] = -INFINITY;
        local_indices[k] = -1;
    }
    
    // Process elements assigned to this thread
    for (int i = tid; i < N; i += block_size) {
        float score = scores[i];
        int idx = i;
        
        // Insert into local top-K
        for (int k = 0; k < K; ++k) {
            if (score > local_scores[k]) {
                for (int j = K - 1; j > k; --j) {
                    local_scores[j] = local_scores[j - 1];
                    local_indices[j] = local_indices[j - 1];
                }
                local_scores[k] = score;
                local_indices[k] = idx;
                break;
            }
        }
    }
    
    // Store local top-K to shared memory
    for (int k = 0; k < K; ++k) {
        s_scores[tid * K + k] = local_scores[k];
        s_indices[tid * K + k] = local_indices[k];
    }
    __syncthreads();
    
    // Thread 0 merges all local top-Ks into global top-K
    if (tid == 0) {
        float merged_scores[8];
        int merged_indices[8];
        for (int k = 0; k < K; ++k) {
            merged_scores[k] = -INFINITY;
            merged_indices[k] = -1;
        }
        
        // Merge from all threads
        for (int t = 0; t < block_size; ++t) {
            for (int k = 0; k < K; ++k) {
                float score = s_scores[t * K + k];
                int idx = s_indices[t * K + k];
                if (idx >= 0) {
                    for (int m = 0; m < K; ++m) {
                        if (score > merged_scores[m]) {
                            for (int j = K - 1; j > m; --j) {
                                merged_scores[j] = merged_scores[j - 1];
                                merged_indices[j] = merged_indices[j - 1];
                            }
                            merged_scores[m] = score;
                            merged_indices[m] = idx;
                            break;
                        }
                    }
                }
            }
        }
        
        // Write results
        for (int k = 0; k < K; ++k) {
            topk_scores[k] = merged_scores[k];
            topk_indices[k] = merged_indices[k];
        }
    }
}

/* ─── Launch Functions ─── */
inline void launch_cosine_sim(
    const float* query, const float* docs, float* scores,
    int N, int D, Arena* /*arena*/, cudaStream_t stream = 0
) {
    const int warps_per_block = 8;
    const int block_size = warps_per_block * 32;
    const int grid_size = (N + warps_per_block - 1) / warps_per_block;
    const size_t smem_size = warps_per_block * sizeof(float);
    
    cosine_sim_kernel<<<grid_size, block_size, smem_size, stream>>>(query, docs, scores, N, D);
}

inline void launch_rrf_topk(
    const float* dense_scores, const float* sparse_scores,
    int N, int K,
    int* topk_indices, float* topk_scores,
    Arena* /*arena*/, cudaStream_t stream = 0
) {
    // Allocate temporary arrays
    int *d_dense_ranks, *d_sparse_ranks;
    float *d_rrf_scores;
    EXM_CUDA_CHECK(cudaMalloc(&d_dense_ranks, N * sizeof(int)));
    EXM_CUDA_CHECK(cudaMalloc(&d_sparse_ranks, N * sizeof(int)));
    EXM_CUDA_CHECK(cudaMalloc(&d_rrf_scores, N * sizeof(float)));
    
    // Initialize top-K arrays
    EXM_CUDA_CHECK(cudaMemset(topk_indices, 0xFF, K * sizeof(int)));  // -1
    EXM_CUDA_CHECK(cudaMemset(topk_scores, 0, K * sizeof(float)));
    
    // Compute ranks (single block, shared memory optimized)
    int rank_block = 1024;
    size_t rank_smem = N * sizeof(float);
    compute_ranks_kernel<<<1, rank_block, rank_smem, stream>>>(dense_scores, N, d_dense_ranks);
    EXM_CUDA_CHECK(cudaGetLastError());
    compute_ranks_kernel<<<1, rank_block, rank_smem, stream>>>(sparse_scores, N, d_sparse_ranks);
    EXM_CUDA_CHECK(cudaGetLastError());
    
    // Compute RRF scores
    int rrf_block = 256;
    int rrf_grid = (N + rrf_block - 1) / rrf_block;
    compute_rrf_kernel<<<rrf_grid, rrf_block, 0, stream>>>(d_dense_ranks, d_sparse_ranks, N, d_rrf_scores);
    EXM_CUDA_CHECK(cudaGetLastError());
    
    // Top-K selection (single block, up to 256 threads to limit shared memory)
    int topk_block = min(256, N);
    size_t topk_smem = topk_block * K * sizeof(float) + topk_block * K * sizeof(int);
    topk_kernel<<<1, topk_block, topk_smem, stream>>>(d_rrf_scores, N, K, topk_indices, topk_scores);
    EXM_CUDA_CHECK(cudaGetLastError());
    
    // Synchronize and cleanup
    EXM_CUDA_CHECK(cudaStreamSynchronize(stream));
    EXM_CUDA_CHECK(cudaFree(d_dense_ranks));
    EXM_CUDA_CHECK(cudaFree(d_sparse_ranks));
    EXM_CUDA_CHECK(cudaFree(d_rrf_scores));
}

#endif /* RRF_TOPK_CUH */