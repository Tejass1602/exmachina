#ifndef EXM_PACKET_H
#define EXM_PACKET_H

#include <stdint.h>
#include <stddef.h>

#pragma pack(push, 1)

/* ─── Packet Header (8 bytes) ─── */
typedef struct {
    uint16_t magic;       /* Fixed 0x4558 ("EX") */
    uint8_t  type;        /* ExmPacketType */
    uint8_t  flags;       /* Bitmask: 0x01 = ACK required, 0x02 = HMAC signed */
    uint16_t payload_len; /* Exact byte size of payload struct (max 512) */
    uint16_t crc16;       /* CRC-16/CCITT-FALSE over [magic..payload_len] (6 bytes) + payload */
} ExmHeader;

/* ─── Packet Type Enum ─── */
typedef enum : uint8_t {
    PKT_AUDIO_FRAME = 0x01,
    PKT_PROBE_INTENT = 0x02,
    PKT_PROBE_CMD = 0x03,
    PKT_HEARTBEAT = 0x04,
} ExmPacketType;

/* ─── Packet Flags ─── */
#define FLAG_ACK_REQUIRED 0x01
#define FLAG_HMAC_SIGNED 0x02

/* ─── Intent ID Enum ─── */
typedef enum : uint8_t {
    INTENT_NONE = 0x00,
    INTENT_SCAN_BUS = 0x01,
    INTENT_CHECK_RAIL = 0x02,
    INTENT_BUILD_FIX = 0x03,
    INTENT_APPLY_PATCH = 0x04,
    INTENT_EMERGENCY_HALT = 0x05,
} ExmIntentId;

/* ─── Probe Operation Enum ─── */
typedef enum : uint8_t {
    PROBE_OP_SCAN_I2C = 0x01,
    PROBE_OP_READ_RAIL = 0x02,
    PROBE_OP_PULSE_STIM = 0x03,
} ExmProbeOp;

/* ─── Payload: Audio Frame (324 bytes) ─── */
typedef struct {
    uint32_t seq;
    int16_t  pcm[160];
} ExmAudioFrame;

/* ─── Payload: Probe Intent (52 bytes) ─── */
/* HMAC-SHA256 computed over the first 20 bytes (up to ripple_mv) */
typedef struct {
    uint8_t  intent_id;      /* ExmIntentId */
    uint8_t  confidence;     /* 0..255 */
    uint32_t seq_num;        /* Monotonic sequence number */
    uint16_t rail_mv;        /* Rail voltage in mV (scaled by 11x from ADC) */
    uint16_t ripple_mv;      /* Rail ripple in mV (scaled by 11x from ADC) */
    uint8_t  i2c_count;      /* Number of I2C addresses found */
    uint8_t  i2c_addrs[8];   /* I2C 7-bit addresses found */
    uint8_t  who_am_i;       /* WHO_AM_I register value */
    uint8_t  hmac_sha256[32];/* HMAC-SHA256 tag (32 bytes) */
} ExmProbeIntent;

/* ─── Payload: Probe Command (4 bytes) ─── */
typedef struct {
    uint8_t  probe_op;   /* ExmProbeOp */
    uint8_t  param0;
    uint16_t param1;
} ExmProbeCmd;

/* ─── Payload: Heartbeat (5 bytes) ─── */
typedef struct {
    uint32_t uptime_ms;
    uint8_t  status_flags;
} ExmHeartbeat;

#pragma pack(pop)

/* ─── Static Assertions ─── */
#ifdef __cplusplus
#define EXM_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define EXM_STATIC_ASSERT(c, m) EXM_STATIC_ASSERT(c, m)
#endif
EXM_STATIC_ASSERT(sizeof(ExmHeader) == 8, "ExmHeader must be 8 bytes");
EXM_STATIC_ASSERT(sizeof(ExmAudioFrame) == 324, "ExmAudioFrame must be 324 bytes");
EXM_STATIC_ASSERT(sizeof(ExmProbeIntent) == 52, "ExmProbeIntent must be 52 bytes");
EXM_STATIC_ASSERT(sizeof(ExmProbeCmd) == 4, "ExmProbeCmd must be 4 bytes");
EXM_STATIC_ASSERT(sizeof(ExmHeartbeat) == 5, "ExmHeartbeat must be 5 bytes");

/* ─── CRC-16/CCITT-FALSE ─── */
/* Polynomial: 0x1021, Initial: 0xFFFF, No reflection, No final XOR */
static inline uint16_t exm_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ─── Constant-time memcmp (for HMAC verification) ─── */
static inline int exm_ct_memcmp(const void* a, const void* b, size_t len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= pa[i] ^ pb[i];
    }
    return diff;
}

#endif /* EXM_PACKET_H */