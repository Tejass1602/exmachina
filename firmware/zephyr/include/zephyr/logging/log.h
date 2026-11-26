#ifndef ZEPHYR_LOGGING_LOG_H
#define ZEPHYR_LOGGING_LOG_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_MODULE_REGISTER(name, level)
#define LOG_INF(fmt, ...) printf("[INF] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) printf("[ERR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) printf("[WRN] " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) printf("[DBG] " fmt "\n", ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_LOGGING_LOG_H */