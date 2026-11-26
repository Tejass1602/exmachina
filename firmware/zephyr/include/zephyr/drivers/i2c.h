#ifndef ZEPHYR_DRIVERS_I2C_H
#define ZEPHYR_DRIVERS_I2C_H

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int i2c_read(const struct device *dev, uint8_t *buf, uint32_t len, uint16_t addr);
int i2c_write(const struct device *dev, const uint8_t *buf, uint32_t len, uint16_t addr);
int i2c_write_read(const struct device *dev, uint16_t addr,
                   const void *write_buf, size_t write_len,
                   void *read_buf, size_t read_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_I2C_H */