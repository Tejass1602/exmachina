#ifndef EXM_INDEXER_H
#define EXM_INDEXER_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <string_view>

/* ─── Chunk Structure ─── */
struct ExmChunk {
    const char* path;        // File path
    const char* symbol;      // Symbol/heading name
    int line_start;          // Starting line (1-based)
    int line_end;            // Ending line (inclusive)
    const char* text;        // Chunk text (owned by indexer)
    int token_count;         // Approximate token count
};

/* ─── Indexer Configuration ─── */
struct ExmIndexerConfig {
    int max_chunk_tokens = 512;
    int min_chunk_tokens = 32;
    float bm25_k1 = 1.5f;
    float bm25_b = 0.75f;
    int embedding_dim = 384;
};

/* ─── Indexer State ─── */
struct ExmIndexer {
    ExmIndexerConfig config;
    std::vector<ExmChunk> chunks;
    std::vector<std::string> chunk_texts;  // Owned text storage
    std::vector<std::string> file_paths;   // Owned path storage
    std::vector<std::string> symbols;      // Owned symbol storage
    
    // BM25 statistics
    std::vector<int> doc_freq;             // Document frequency per term
    int num_docs = 0;
    float avg_doc_len = 0.0f;
    
    // Feature hashing embeddings
    std::vector<float> embeddings;         // [num_chunks * embedding_dim]
};

/* ─── Initialize Indexer ─── */
void exm_indexer_init(ExmIndexer* idx, const ExmIndexerConfig* config = nullptr);

/* ─── Add File to Index ─── */
/* Parses file into chunks based on language (C/C++/CUDA/Markdown) */
int exm_indexer_add_file(ExmIndexer* idx, const char* filepath);

/* ─── Build BM25 Statistics ─── */
/* Computes document frequencies and average document length */
void exm_indexer_build_bm25(ExmIndexer* idx);

/* ─── Compute BM25 Scores ─── */
/* Computes BM25 scores for a query against all chunks */
void exm_indexer_bm25_scores(const ExmIndexer* idx, const char* query, float* scores);

/* ─── Feature Hashing Embedding ─── */
/* Computes 384-dim L2-normalized feature-hashing embedding for a query */
void exm_indexer_embed_query(const ExmIndexer* idx, const char* query, float* embedding);

/* ─── Get Chunk Count ─── */
int exm_indexer_chunk_count(const ExmIndexer* idx);

/* ─── Get Chunk ─── */
const ExmChunk* exm_indexer_get_chunk(const ExmIndexer* idx, int chunk_idx);

/* ─── Cleanup ─── */
void exm_indexer_free(ExmIndexer* idx);

#endif /* EXM_INDEXER_H */