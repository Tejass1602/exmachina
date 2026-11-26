#ifndef ZEPHYR_DEVICE_H
#define ZEPHYR_DEVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct device;
#define DEVICE_DT_GET(node) ((const struct device *)0x1)

int device_is_ready(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DEVICE_H */