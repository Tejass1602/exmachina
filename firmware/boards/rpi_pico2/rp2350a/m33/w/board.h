/* ─── RP2350A Pico 2 W Board Header ─── */

#ifndef _BOARD_H_
#define _BOARD_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

/* ─── LED ─── */
#define LED0_NODE DT_ALIAS(led0)
#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED0	DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED0_PIN	DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_FLAGS	DT_GPIO_FLAGS(LED0_NODE, gpios)
#endif

/* ─── User Button (BOOTSEL) ─── */
#define SW0_NODE DT_ALIAS(sw0)
#if DT_NODE_HAS_STATUS(SW0_NODE, okay)
#define SW0_GPIO_LABEL	DT_GPIO_LABEL(SW0_NODE, gpios)
#define SW0_GPIO_PIN	DT_GPIO_PIN(SW0_NODE, gpios)
#define SW0_GPIO_FLAGS	DT_GPIO_FLAGS(SW0_NODE, gpios)
#endif

/* ─── ExMachina Probe Pins ─── */
/* GP0/GP1 - I2C0 (Digital Bus Sniffer) */
#define I2C0_SDA_PIN	0
#define I2C0_SCL_PIN	1

/* GP26 - ADC0 (Rail Voltage & Ripple Probe) */
#define ADC0_PIN	26
#define ADC0_CHANNEL	0

/* GP27 - ADC1 (Component/Continuity Probe) */
#define ADC1_PIN	27
#define ADC1_CHANNEL	1

/* WL_GPIO0 - Status/Watchdog LED */
#define STATUS_LED_PIN	23  /* WL_GPIO0 on Pico 2 W */

/* ─── USB ─── */
#define USB_DEVICE_VID	0x2E8A
#define USB_DEVICE_PID	0x000A

#endif /* _BOARD_H_ */