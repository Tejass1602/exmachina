#ifndef ZEPHYR_SYS_CRC_H
#define ZEPHYR_SYS_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t crc16(uint16_t crc, const void *buf, size_t len, uint16_t poly, bool reflect_in, bool reflect_out);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SYS_CRC_H */