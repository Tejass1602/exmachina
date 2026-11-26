#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <cstdint>
#include <cstddef>
#include <cstring>

/* ─── SHA-256 Constants ─── */
static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* ─── SHA-256 Context ─── */
struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

/* ─── SHA-256 Functions ─── */
static inline void sha256_init(Sha256Ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    std::memset(ctx->buffer, 0, 64);
}

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n)  ((x) >> (n))
#define SIGMA0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIGMA1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sigma0(x)  (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define sigma1(x)  (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

static void sha256_transform(Sha256Ctx* ctx, const uint8_t* block) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    
    // Prepare message schedule
    for (int i = 0; i < 16; ++i) {
        W[i] = (uint32_t)block[i * 4] << 24 |
               (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];
    }
    
    // Initialize working variables
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    
    // Main loop
    for (int i = 0; i < 64; ++i) {
        uint32_t T1 = h + SIGMA1(e) + CH(e, f, g) + SHA256_K[i] + W[i];
        uint32_t T2 = SIGMA0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }
    
    // Update state
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    size_t idx = (size_t)(ctx->count % 64);
    ctx->count += len;
    
    while (len > 0) {
        size_t space = 64 - idx;
        size_t copy = (len < space) ? len : space;
        std::memcpy(ctx->buffer + idx, data, copy);
        
        if (idx + copy == 64) {
            sha256_transform(ctx, ctx->buffer);
            idx = 0;
        } else {
            idx += copy;
        }
        data += copy;
        len -= copy;
    }
}

static void sha256_final(Sha256Ctx* ctx, uint8_t* hash) {
    size_t idx = (size_t)(ctx->count % 64);
    
    // Padding
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        std::memset(ctx->buffer + idx, 0, 64 - idx);
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    std::memset(ctx->buffer + idx, 0, 56 - idx);
    
    // Append bit length (big-endian)
    uint64_t bit_len = ctx->count * 8;
    for (int i = 0; i < 8; ++i) {
        ctx->buffer[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    
    sha256_transform(ctx, ctx->buffer);
    
    // Output hash (big-endian)
    for (int i = 0; i < 8; ++i) {
        hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* ─── HMAC-SHA256 (RFC 2104) ─── */
static void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t* output) {
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t temp_hash[32];
    Sha256Ctx ctx;
    
    // If key is longer than 64 bytes, hash it first
    if (key_len > 64) {
        Sha256Ctx tmp;
        sha256_init(&tmp);
        sha256_update(&tmp, key, key_len);
        sha256_final(&tmp, k_ipad);
        key = k_ipad;
        key_len = 32;
    }
    
    // Prepare inner and outer pads
    std::memset(k_ipad, 0x36, 64);
    std::memset(k_opad, 0x5C, 64);
    for (size_t i = 0; i < key_len; ++i) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }
    
    // Inner hash
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, temp_hash);
    
    // Outer hash
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, temp_hash, 32);
    sha256_final(&ctx, output);
}

/* ─── Verify HMAC on ExmProbeIntent ─── */
/* Computes HMAC over first 20 bytes of ExmProbeIntent (intent_id through ripple_mv) */
static inline void exm_probe_intent_compute_hmac(const uint8_t* key, size_t key_len,
                                                  const uint8_t* intent_data,
                                                  uint8_t* hmac_out) {
    // HMAC over first 20 bytes: intent_id(1) + confidence(1) + seq_num(4) + rail_mv(2) + ripple_mv(2) + i2c_count(1) + i2c_addrs[8](8) + who_am_i(1) = 20
    hmac_sha256(key, key_len, intent_data, 20, hmac_out);
}

/* ─── RFC 4231 Test Vectors ─── */
static bool sha256_self_test() {
    // Test vector 1: "abc"
    const uint8_t test1[] = "abc";
    uint8_t hash1[32];
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, test1, 3);
    sha256_final(&ctx, hash1);
    
    static const uint8_t expected1[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    if (exm_ct_memcmp(hash1, expected1, 32) != 0) return false;
    
    // Test vector 2: HMAC-SHA256 with key "key" and data "The quick brown fox jumps over the lazy dog"
    const uint8_t key2[] = "key";
    const uint8_t data2[] = "The quick brown fox jumps over the lazy dog";
    uint8_t hmac2[32];
    hmac_sha256(key2, 3, data2, 43, hmac2);
    
    static const uint8_t expected2[32] = {
        0xf7, 0xbc, 0x83, 0xf4, 0x30, 0x53, 0x84, 0x24,
        0xb1, 0x32, 0x98, 0xe6, 0xaa, 0x6f, 0xb1, 0x43,
        0xef, 0x4d, 0x59, 0xa1, 0x49, 0x29, 0xac, 0x68,
        0x19, 0x17, 0x4c, 0x8d, 0x7d, 0x8c, 0x6e, 0x87
    };
    if (exm_ct_memcmp(hmac2, expected2, 32) != 0) return false;
    
    return true;
}

#endif /* HMAC_SHA256_H */