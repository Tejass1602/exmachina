#ifndef SANDBOX_H
#define SANDBOX_H

#include <cstddef>

/* ─── Sandbox Result ─── */
struct SandboxResult {
    bool success;
    char error[512];
};

/* ─── Apply Unified Diff and Compile ─── */
/* 
 * Creates temporary directory /tmp/exm-sandbox-XXXXXX
 * Copies target files, applies unified diff, runs nvcc/g++ verification
 * Cleans up on exit
 */
SandboxResult sandbox_apply_and_compile(const char* diff_text, size_t diff_len);

#endif /* SANDBOX_H */