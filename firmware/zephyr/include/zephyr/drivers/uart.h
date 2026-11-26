#ifndef ZEPHYR_DRIVERS_UART_H
#define ZEPHYR_DRIVERS_UART_H

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uart_irq_callback_user_data_t)(const struct device *dev, void *user_data);

void uart_irq_callback_user_data_set(const struct device *dev, uart_irq_callback_user_data_t cb, void *user_data);
void uart_irq_rx_enable(const struct device *dev);
int uart_poll_in(const struct device *dev, unsigned char *c);
int uart_tx(const struct device *dev, const uint8_t *buf, size_t len, int32_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_UART_H */