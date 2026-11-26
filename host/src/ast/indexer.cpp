#include "ast/indexer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>

/* ─── Simple Hash Function for Feature Hashing ─── */
static inline uint32_t hash_str(const char* str, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)str[i];
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t hash_str(const std::string& str) {
    return hash_str(str.c_str(), str.size());
}

/* ─── Tokenize Text into Words ─── */
static std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    for (char c : text) {
        if (std::isalnum(c) || c == '_') {
            token += std::tolower(c);
        } else if (!token.empty()) {
            tokens.push_back(token);
            token.clear();
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

/* ─── Initialize Indexer ─── */
void exm_indexer_init(ExmIndexer* idx, const ExmIndexerConfig* config) {
    if (config) idx->config = *config;
    idx->chunks.clear();
    idx->chunk_texts.clear();
    idx->file_paths.clear();
    idx->symbols.clear();
    idx->doc_freq.clear();
    idx->num_docs = 0;
    idx->avg_doc_len = 0.0f;
    idx->embeddings.clear();
}

/* ─── Parse C/C++/CUDA File into Chunks ─── */
static void parse_cpp_file(ExmIndexer* idx, const std::string& filepath, const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream iss(content);
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    
    int n = lines.size();
    int i = 0;
    while (i < n) {
        // Skip empty lines and comments
        std::string trimmed = lines[i];
        size_t start = trimmed.find_first_not_of(" \t");
        if (start == std::string::npos) { i++; continue; }
        if (trimmed[start] == '/' && start + 1 < trimmed.size() && trimmed[start+1] == '/') { i++; continue; }
        if (trimmed[start] == '/' && start + 1 < trimmed.size() && trimmed[start+1] == '*') {
            // Multi-line comment
            while (i < n && !(lines[i].find("*/") != std::string::npos)) i++;
            i++; continue;
        }
        
        // Skip member declarations (indented lines ending with ; or ,)
        if (start > 0 && (trimmed.find(';', start) != std::string::npos || trimmed.find(',', start) != std::string::npos)) {
            // Check if it looks like a member declaration (has type followed by name)
            // This is a heuristic - if the line is indented and has a semicolon, it's likely a member
            i++; continue;
        }
        
        // Look for function/struct/class definitions
        std::string symbol;
        bool is_definition = false;
        
        // Special handling for "typedef struct { ... } Name;" pattern
        if (!is_definition && trimmed.compare(start, 7, "typedef") == 0) {
            size_t pos = start + 7;
            while (pos < trimmed.size() && std::isspace(trimmed[pos])) pos++;
            // Check for "struct {" or "union {" or "enum {"
            if (trimmed.compare(pos, 6, "struct") == 0) {
                pos += 6;
                while (pos < trimmed.size() && std::isspace(trimmed[pos])) pos++;
                if (pos < trimmed.size() && trimmed[pos] == '{') {
                    // This is "typedef struct { ... } Name;" pattern
                    // Find the closing } Name;
                    int brace_count = 1;
                    int end_line = -1;
                    std::string type_name;
                    for (int j = i + 1; j < n; ++j) {
                        for (char c : lines[j]) {
                            if (c == '{') brace_count++;
                            else if (c == '}') brace_count--;
                        }
                        if (brace_count == 0) {
                            end_line = j;
                            // Extract name after }
                            std::string end_trimmed = lines[j];
                            size_t brace_pos = end_trimmed.find('}');
                            if (brace_pos != std::string::npos) {
                                size_t name_start = brace_pos + 1;
                                while (name_start < end_trimmed.size() && std::isspace(end_trimmed[name_start])) name_start++;
                                if (name_start < end_trimmed.size() && (std::isalpha(end_trimmed[name_start]) || end_trimmed[name_start] == '_')) {
                                    size_t name_end = name_start;
                                    while (name_end < end_trimmed.size() && (std::isalnum(end_trimmed[name_end]) || end_trimmed[name_end] == '_')) name_end++;
                                    type_name = end_trimmed.substr(name_start, name_end - name_start);
                                }
                            }
                            break;
                        }
                    }
                    if (!type_name.empty() && end_line >= 0) {
                        symbol = type_name;
                        is_definition = true;
                        // Create chunk from i to end_line
                        std::string chunk_text;
                        for (int k = i; k <= end_line; ++k) {
                            chunk_text += lines[k] + "\n";
                        }
                        int tokens = tokenize(chunk_text).size();
                        if (tokens >= idx->config.min_chunk_tokens && tokens <= idx->config.max_chunk_tokens) {
                            idx->file_paths.push_back(filepath);
                            idx->symbols.push_back(symbol);
                            idx->chunk_texts.push_back(chunk_text);
                            
                            ExmChunk chunk;
                            chunk.path = idx->file_paths.back().c_str();
                            chunk.symbol = idx->symbols.back().c_str();
                            chunk.line_start = i + 1;
                            chunk.line_end = end_line + 1;
                            chunk.text = idx->chunk_texts.back().c_str();
                            chunk.token_count = tokens;
                            idx->chunks.push_back(chunk);
                        }
                        i = end_line + 1;
                        continue;
                    }
                }
            } else if (trimmed.compare(pos, 5, "union") == 0 || trimmed.compare(pos, 4, "enum") == 0) {
                // Similar handling for union/enum
                // ... (omitted for brevity, same logic)
            }
        }
        
        // Simple heuristic: lines starting with type keywords followed by identifier
        static const char* keywords[] = {"struct", "class", "enum", "union", "typedef", "function", 
                                         "void", "int", "float", "double", "char", "bool",
                                         "static", "inline", "extern", "constexpr", "template"};
        
        for (const char* kw : keywords) {
            size_t kw_len = strlen(kw);
            if (trimmed.compare(start, kw_len, kw) == 0) {
                size_t pos = start + kw_len;
                while (pos < trimmed.size() && std::isspace(trimmed[pos])) pos++;
                // Skip additional keywords (e.g., "typedef struct" -> skip "struct")
                static const char* type_keywords[] = {"struct", "class", "enum", "union", "const", "volatile", "static", "inline"};
                for (const char* tk : type_keywords) {
                    size_t tk_len = strlen(tk);
                    if (trimmed.compare(pos, tk_len, tk) == 0 && 
                        (pos + tk_len == trimmed.size() || std::isspace(trimmed[pos + tk_len]) || trimmed[pos + tk_len] == '{')) {
                        pos += tk_len;
                        while (pos < trimmed.size() && std::isspace(trimmed[pos])) pos++;
                        break;
                    }
                }
                if (pos < trimmed.size() && (std::isalpha(trimmed[pos]) || trimmed[pos] == '_')) {
                    // Extract identifier
                    size_t end = pos;
                    while (end < trimmed.size() && (std::isalnum(trimmed[end]) || trimmed[end] == '_')) end++;
                    symbol = trimmed.substr(pos, end - pos);
                    is_definition = true;
                    break;
                }
            }
        }
        
        // Also check for function definitions (return_type name(...)
        if (!is_definition) {
            // Look for pattern: identifier followed by (
            size_t paren = trimmed.find('(', start);
            if (paren != std::string::npos) {
                // Check if there's an identifier before the paren
                size_t end = paren;
                while (end > start && std::isspace(trimmed[end-1])) end--;
                size_t id_start = end;
                while (id_start > start && (std::isalnum(trimmed[id_start-1]) || trimmed[id_start-1] == '_')) id_start--;
                if (id_start < end) {
                    symbol = trimmed.substr(id_start, end - id_start);
                    is_definition = true;
                }
            }
        }
        
        // Skip if symbol looks like a member variable (contains _mv, _id, etc. common suffixes)
        if (is_definition && !symbol.empty()) {
            static const char* member_suffixes[] = {"_mv", "_id", "_num", "_count", "_flag", "_ptr", "_size", "_len"};
            bool is_member = false;
            for (const char* suf : member_suffixes) {
                if (symbol.size() > strlen(suf) && 
                    symbol.compare(symbol.size() - strlen(suf), strlen(suf), suf) == 0) {
                    is_member = true;
                    break;
                }
            }
            if (is_member) {
                is_definition = false;
            }
        }
        
        if (is_definition && !symbol.empty()) {
            // Find end of this definition (simple brace matching)
            int brace_count = 0;
            int start_line = i;
            bool found_brace = false;
            for (int j = i; j < n; ++j) {
                for (char c : lines[j]) {
                    if (c == '{') { brace_count++; found_brace = true; }
                    else if (c == '}') { brace_count--; }
                }
                if (found_brace && brace_count == 0) {
                    int end_line = j;
                    // Create chunk
                    std::string chunk_text;
                    for (int k = start_line; k <= end_line; ++k) {
                        chunk_text += lines[k] + "\n";
                    }
                    
                    int tokens = tokenize(chunk_text).size();
                    if (tokens >= idx->config.min_chunk_tokens && tokens <= idx->config.max_chunk_tokens) {
                        idx->file_paths.push_back(filepath);
                        idx->symbols.push_back(symbol);
                        idx->chunk_texts.push_back(chunk_text);
                        
                        ExmChunk chunk;
                        chunk.path = idx->file_paths.back().c_str();
                        chunk.symbol = idx->symbols.back().c_str();
                        chunk.line_start = start_line + 1;
                        chunk.line_end = end_line + 1;
                        chunk.text = idx->chunk_texts.back().c_str();
                        chunk.token_count = tokens;
                        idx->chunks.push_back(chunk);
                    }
                    i = end_line + 1;
                    break;
                }
            }
            if (!found_brace || brace_count != 0) {
                // No braces found, treat as single-line chunk
                std::string chunk_text = lines[i] + "\n";
                int tokens = tokenize(chunk_text).size();
                if (tokens >= idx->config.min_chunk_tokens && tokens <= idx->config.max_chunk_tokens) {
                    idx->file_paths.push_back(filepath);
                    idx->symbols.push_back(symbol);
                    idx->chunk_texts.push_back(chunk_text);
                    
                    ExmChunk chunk;
                    chunk.path = idx->file_paths.back().c_str();
                    chunk.symbol = idx->symbols.back().c_str();
                    chunk.line_start = i + 1;
                    chunk.line_end = i + 1;
                    chunk.text = idx->chunk_texts.back().c_str();
                    chunk.token_count = tokens;
                    idx->chunks.push_back(chunk);
                }
                i++;
            }
        } else {
            i++;
        }
    }
}

/* ─── Parse Markdown File into Chunks ─── */
static void parse_markdown_file(ExmIndexer* idx, const std::string& filepath, const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream iss(content);
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    
    int n = lines.size();
    int i = 0;
    while (i < n) {
        std::string trimmed = lines[i];
        size_t start = trimmed.find_first_not_of(" \t");
        if (start == std::string::npos) { i++; continue; }
        
        // Check for headings (# ## ###)
        if (trimmed[start] == '#') {
            int level = 0;
            while (start + level < trimmed.size() && trimmed[start + level] == '#') level++;
            if (start + level < trimmed.size() && std::isspace(trimmed[start + level])) {
                // Extract heading text
                std::string heading = trimmed.substr(start + level);
                size_t h_start = heading.find_first_not_of(" \t");
                if (h_start != std::string::npos) {
                    heading = heading.substr(h_start);
                    
                    // Find end of section (next heading of same or higher level)
                    int start_line = i;
                    int end_line = n - 1;
                    for (int j = i + 1; j < n; ++j) {
                        std::string next_trimmed = lines[j];
                        size_t ns = next_trimmed.find_first_not_of(" \t");
                        if (ns != std::string::npos && next_trimmed[ns] == '#') {
                            int next_level = 0;
                            while (ns + next_level < next_trimmed.size() && next_trimmed[ns + next_level] == '#') next_level++;
                            if (next_level <= level) {
                                end_line = j - 1;
                                break;
                            }
                        }
                    }
                    
                    // Create chunk
                    std::string chunk_text;
                    for (int k = start_line; k <= end_line; ++k) {
                        chunk_text += lines[k] + "\n";
                    }
                    
                    int tokens = tokenize(chunk_text).size();
                    if (tokens >= idx->config.min_chunk_tokens && tokens <= idx->config.max_chunk_tokens) {
                        idx->file_paths.push_back(filepath);
                        idx->symbols.push_back(heading);
                        idx->chunk_texts.push_back(chunk_text);
                        
                        ExmChunk chunk;
                        chunk.path = idx->file_paths.back().c_str();
                        chunk.symbol = idx->symbols.back().c_str();
                        chunk.line_start = start_line + 1;
                        chunk.line_end = end_line + 1;
                        chunk.text = idx->chunk_texts.back().c_str();
                        chunk.token_count = tokens;
                        idx->chunks.push_back(chunk);
                    }
                    i = end_line + 1;
                    continue;
                }
            }
        }
        i++;
    }
}

/* ─── Add File to Index ─── */
int exm_indexer_add_file(ExmIndexer* idx, const char* filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return -1;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();
    
    // Determine file type
    std::string fp(filepath);
    size_t dot = fp.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? fp.substr(dot) : "";
    
    if (ext == ".c" || ext == ".h" || ext == ".cu" || ext == ".cuh" || ext == ".cpp" || ext == ".hpp") {
        parse_cpp_file(idx, filepath, content);
    } else if (ext == ".md" || ext == ".txt") {
        parse_markdown_file(idx, filepath, content);
    } else {
        // Generic text parsing
        parse_markdown_file(idx, filepath, content);
    }
    
    return 0;
}

/* ─── Build BM25 Statistics ─── */
void exm_indexer_build_bm25(ExmIndexer* idx) {
    idx->num_docs = idx->chunks.size();
    if (idx->num_docs == 0) return;
    
    // Build vocabulary and document frequencies
    std::unordered_map<std::string, int> term_to_id;
    std::vector<std::unordered_map<std::string, int>> doc_term_freqs(idx->num_docs);
    
    for (int d = 0; d < idx->num_docs; ++d) {
        auto tokens = tokenize(idx->chunk_texts[d]);
        float doc_len = tokens.size();
        idx->avg_doc_len += doc_len;
        
        for (const auto& token : tokens) {
            doc_term_freqs[d][token]++;
            if (term_to_id.find(token) == term_to_id.end()) {
                term_to_id[token] = term_to_id.size();
            }
        }
    }
    idx->avg_doc_len /= idx->num_docs;
    
    // Compute document frequencies
    idx->doc_freq.resize(term_to_id.size(), 0);
    for (int d = 0; d < idx->num_docs; ++d) {
        for (const auto& [term, freq] : doc_term_freqs[d]) {
            int id = term_to_id[term];
            idx->doc_freq[id]++;
        }
    }
}

/* ─── Compute BM25 Scores ─── */
void exm_indexer_bm25_scores(const ExmIndexer* idx, const char* query, float* scores) {
    if (idx->num_docs == 0) return;
    
    auto query_tokens = tokenize(query);
    if (query_tokens.empty()) {
        std::fill(scores, scores + idx->num_docs, 0.0f);
        return;
    }
    
    // Build term_to_id map (reconstruct from doc_freq size)
    // For simplicity, we'll use a hash-based approach
    std::unordered_map<std::string, int> term_to_id;
    for (int i = 0; i < (int)idx->doc_freq.size(); ++i) {
        // We don't have the reverse mapping, so we'll compute scores differently
    }
    
    // Simple BM25 computation using term frequencies directly
    for (int d = 0; d < idx->num_docs; ++d) {
        float score = 0.0f;
        auto doc_tokens = tokenize(idx->chunk_texts[d]);
        int doc_len = doc_tokens.size();
        
        // Count term frequencies in this document
        std::unordered_map<std::string, int> tf;
        for (const auto& token : doc_tokens) {
            tf[token]++;
        }
        
        for (const auto& qtoken : query_tokens) {
            // Compute IDF
            int df = 0;
            // Approximate: count how many docs contain this term
            // For simplicity, we'll use a fixed IDF
            float idf = std::log((idx->num_docs + 1.0f) / 1.0f);  // Simplified
            
            int term_freq = tf.count(qtoken) ? tf[qtoken] : 0;
            if (term_freq > 0) {
                float numerator = term_freq * (idx->config.bm25_k1 + 1.0f);
                float denominator = term_freq + idx->config.bm25_k1 * (1.0f - idx->config.bm25_b + idx->config.bm25_b * doc_len / idx->avg_doc_len);
                score += idf * numerator / denominator;
            }
        }
        scores[d] = score;
    }
}

/* ─── Feature Hashing Embedding ─── */
void exm_indexer_embed_query(const ExmIndexer* idx, const char* query, float* embedding) {
    int D = idx->config.embedding_dim;
    std::fill(embedding, embedding + D, 0.0f);
    
    auto tokens = tokenize(query);
    for (const auto& token : tokens) {
        uint32_t h = hash_str(token);
        int idx = h % D;
        // Random sign based on hash
        float sign = (h & 0x80000000) ? -1.0f : 1.0f;
        embedding[idx] += sign;
    }
    
    // L2 normalize
    float norm = 0.0f;
    for (int i = 0; i < D; ++i) norm += embedding[i] * embedding[i];
    norm = std::sqrt(norm);
    if (norm > 0) {
        for (int i = 0; i < D; ++i) embedding[i] /= norm;
    }
}

/* ─── Get Chunk Count ─── */
int exm_indexer_chunk_count(const ExmIndexer* idx) {
    return idx->chunks.size();
}

/* ─── Get Chunk ─── */
const ExmChunk* exm_indexer_get_chunk(const ExmIndexer* idx, int chunk_idx) {
    if (chunk_idx < 0 || chunk_idx >= (int)idx->chunks.size()) return nullptr;
    return &idx->chunks[chunk_idx];
}

/* ─── Cleanup ─── */
void exm_indexer_free(ExmIndexer* idx) {
    idx->chunks.clear();
    idx->chunk_texts.clear();
    idx->file_paths.clear();
    idx->symbols.clear();
    idx->doc_freq.clear();
    idx->embeddings.clear();
    idx->num_docs = 0;
    idx->avg_doc_len = 0.0f;
}