#ifndef ZEPHYR_DRIVERS_GPIO_H
#define ZEPHYR_DRIVERS_GPIO_H

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_ACTIVE_HIGH 0
#define GPIO_ACTIVE_LOW 1
#define GPIO_OUTPUT_ACTIVE 1
#define GPIO_OUTPUT_INACTIVE 0
#define GPIO_INPUT 0
#define GPIO_OUTPUT 1

struct gpio_dt_spec {
    const struct device *port;
    uint32_t pin;
    uint32_t flags;
};

#define GPIO_DT_SPEC_GET(node, prop) {0, 0, 0}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags);
int gpio_pin_toggle_dt(const struct gpio_dt_spec *spec);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_GPIO_H */