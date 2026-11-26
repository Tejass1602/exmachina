#ifndef ZEPHYR_USB_DEVICE_H
#define ZEPHYR_USB_DEVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int usb_enable(void *config);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_USB_DEVICE_H */