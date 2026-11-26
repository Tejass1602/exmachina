/* ─── ExMachina Smart Bench Probe Firmware ─── */
/* Target: Raspberry Pi Pico 2 W (RP2350A, Cortex-M33) */
/* Implements: I2C scan, Rail probe, USB CDC + HMAC intent signing */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include "../../shared/exm_packet.h"

/* ─── Logging ─── */
LOG_MODULE_REGISTER(exm_probe, LOG_LEVEL_INF);

/* ─── Pin Definitions ─── */
#define I2C0_NODE		DT_NODELABEL(i2c0)
#define ADC_NODE		DT_NODELABEL(adc)
#define STATUS_LED_PIN		23  /* WL_GPIO0 */
#define ADC0_CHANNEL		0   /* GP26 */
#define ADC1_CHANNEL		1   /* GP27 */

/* ─── USB CDC ─── */
#define CDC_ACM_CLASS		0x02
#define USB_BUF_SIZE		512

/* ─── Global State ─── */
static const struct device *i2c0_dev;
static const struct device *adc_dev;
static const struct device *uart_dev;
static struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct k_work_q probe_work_q;
static uint32_t seq_num = 0;
static uint32_t last_heartbeat = 0;
static uint8_t hmac_key[32] = {0x42};  /* EXM_DEV_HMAC_KEY - OTP in production */

/* ─── Ring Buffers ─── */
static uint8_t audio_ring[2][320];  /* 2 x 10ms frames @ 16kHz 16-bit mono */
static volatile int audio_write_idx = 0;
static volatile int audio_read_idx = 0;
static volatile bool audio_ring_full = false;

/* ─── HMAC-SHA256 (simplified for firmware - use mbedTLS in production) ─── */
static void hmac_sha256_fw(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t *output) {
    /* In production, use mbedTLS or hardware acceleration.
     * For prototype, use software implementation from host. */
    /* Placeholder - compile with CONFIG_MBEDTLS for real HMAC */
    (void)key; (void)key_len; (void)data; (void)data_len;
    memset(output, 0xAA, 32);  /* Dummy HMAC for prototype */
}

/* ─── CRC-16 ─── */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    return crc16(0xFFFF, data, len, 0x1021, false, false);
}

/* ─── Packet Send ─── */
static int send_packet(uint8_t type, uint8_t flags, const void *payload, size_t payload_len) {
    if (payload_len > 512) return -EINVAL;

    ExmHeader hdr = {
        .magic = 0x4558,
        .type = type,
        .flags = flags,
        .payload_len = (uint16_t)payload_len,
        .crc16 = 0
    };

    uint8_t crc_buf[6 + 512];
    memcpy(crc_buf, &hdr, 6);
    memcpy(crc_buf + 6, payload, payload_len);
    hdr.crc16 = crc16_ccitt(crc_buf, 6 + payload_len);

    uint8_t packet[8 + 512];
    memcpy(packet, &hdr, 8);
    memcpy(packet + 8, payload, payload_len);

    int written = uart_tx(uart_dev, packet, 8 + payload_len, SYS_FOREVER_US);
    return written;
}

/* ─── Probe: I2C Scan ─── */
static void probe_scan_i2c(void) {
    ExmProbeIntent intent = {0};
    intent.intent_id = INTENT_SCAN_BUS;
    intent.confidence = 255;
    intent.seq_num = ++seq_num;

    uint8_t found_addrs[8] = {0};
    uint8_t count = 0;

    LOG_INF("Scanning I2C bus 0x08..0x77");
    for (uint8_t addr = 0x08; addr <= 0x77 && count < 8; addr++) {
        uint8_t dummy;
        int ret = i2c_read(i2c0_dev, &dummy, 1, addr);
        if (ret == 0) {
            found_addrs[count++] = addr;
            LOG_INF("Found device at 0x%02X", addr);
        }
    }

    intent.i2c_count = count;
    memcpy(intent.i2c_addrs, found_addrs, count);

    /* Try common WHO_AM_I registers for first device */
    if (count > 0) {
        uint8_t who_am_i = 0;
        for (int i = 0; i < 3; i++) {
            static const uint8_t who_regs[3] = {0x0F, 0x75, 0xD0};
            uint8_t reg = who_regs[i];
            int ret = i2c_write_read(i2c0_dev, found_addrs[0], &reg, 1, &who_am_i, 1);
            if (ret == 0) {
                intent.who_am_i = who_am_i;
                LOG_INF("Device 0x%02X WHO_AM_I = 0x%02X", found_addrs[0], who_am_i);
                break;
            }
        }
    }

    /* Compute HMAC over first 20 bytes */
    hmac_sha256_fw(hmac_key, 32, (uint8_t *)&intent, 20, intent.hmac_sha256);

    send_packet(PKT_PROBE_INTENT, FLAG_HMAC_SIGNED, &intent, sizeof(intent));
}

/* ─── Probe: Rail Voltage ─── */
static void probe_read_rail(void) {
    ExmProbeIntent intent = {0};
    intent.intent_id = INTENT_CHECK_RAIL;
    intent.confidence = 200;
    intent.seq_num = ++seq_num;

    /* ADC setup for GP26 (ADC0) */
    struct adc_channel_cfg cfg = {
        .gain = ADC_GAIN_1,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id = ADC0_CHANNEL,
        .differential = 0
    };
    adc_channel_setup(adc_dev, &cfg);

    /* Sample 256 times */
    int16_t samples[256];
    struct adc_sequence seq = {
        .channels = BIT(ADC0_CHANNEL),
        .buffer = samples,
        .buffer_size = sizeof(samples),
        .resolution = 12,
        .oversampling = 0,
        .calibrate = true
    };

    int sum = 0;
    int min_val = 4095, max_val = 0;

    for (int i = 0; i < 256; i++) {
        adc_read(adc_dev, &seq);
        int val = samples[0];
        sum += val;
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
        k_usleep(8);  /* ~2ms total */
    }

    /* Convert to mV: 12-bit ADC, 3.3V ref, 10:1 divider = *11 */
    int mean = sum / 256;
    intent.rail_mv = (uint16_t)((mean * 3300 * 11) / 4096);
    intent.ripple_mv = (uint16_t)(((max_val - min_val) * 3300 * 11) / 4096);

    LOG_INF("Rail: %dmV, Ripple: %dmV", intent.rail_mv, intent.ripple_mv);

    hmac_sha256_fw(hmac_key, 32, (uint8_t *)&intent, 20, intent.hmac_sha256);
    send_packet(PKT_PROBE_INTENT, FLAG_HMAC_SIGNED, &intent, sizeof(intent));
}

/* ─── Probe: Pulse Stimulus (stub) ─── */
static void probe_pulse_stim(uint8_t param0, uint16_t param1) {
    LOG_INF("Pulse stim: param0=%u, param1=%u", param0, param1);
    /* TODO: Implement GPIO pulse on GP22 or similar */
}

/* ─── USB CDC RX Handler ─── */
static void cdc_acm_rx_cb(const struct device *dev, void *user_data) {
    static uint8_t rx_buf[8 + 512];
    static size_t rx_pos = 0;
    static ExmHeader hdr;
    static bool header_done = false;
    static size_t payload_needed = 0;

    while (uart_poll_in(dev, &rx_buf[rx_pos]) == 0) {
        if (!header_done) {
            if (rx_pos < sizeof(ExmHeader)) {
                rx_pos++;
                if (rx_pos == sizeof(ExmHeader)) {
                    memcpy(&hdr, rx_buf, sizeof(ExmHeader));
                    if (hdr.magic != 0x4558 || hdr.payload_len > 512) {
                        rx_pos = 0;
                        continue;
                    }
                    header_done = true;
                    payload_needed = hdr.payload_len;
                    rx_pos = 0;
                }
            }
        } else {
            if (rx_pos < payload_needed) {
                rx_pos++;
                if (rx_pos == payload_needed) {
                    /* Verify CRC */
                    uint8_t crc_buf[6 + 512];
                    memcpy(crc_buf, &hdr, 6);
                    memcpy(crc_buf + 6, rx_buf, payload_needed);
                    if (crc16_ccitt(crc_buf, 6 + payload_needed) == hdr.crc16) {
                        /* Handle packet */
                        if (hdr.type == PKT_PROBE_CMD && payload_needed == sizeof(ExmProbeCmd)) {
                            ExmProbeCmd *cmd = (ExmProbeCmd *)rx_buf;
                            switch (cmd->probe_op) {
                                case PROBE_OP_SCAN_I2C:
                                    probe_scan_i2c();
                                    break;
                                case PROBE_OP_READ_RAIL:
                                    probe_read_rail();
                                    break;
                                case PROBE_OP_PULSE_STIM:
                                    probe_pulse_stim(cmd->param0, cmd->param1);
                                    break;
                            }
                        }
                    }
                    header_done = false;
                    rx_pos = 0;
                }
            }
        }
    }
}

/* ─── Audio Ring Buffer (for KWS) ─── */
static void audio_ring_push(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        audio_ring[audio_write_idx][audio_read_idx] = data[i];
        audio_read_idx = (audio_read_idx + 1) % 320;
        if (audio_read_idx == 0) {
            audio_write_idx = 1 - audio_write_idx;
            audio_ring_full = true;
        }
    }
}

/* ─── Heartbeat Timer ─── */
static struct k_timer hb_timer;
static void heartbeat_timer(struct k_timer *timer) {
    (void)timer;
    ExmHeartbeat hb = {
        .uptime_ms = k_uptime_get_32(),
        .status_flags = 0x01
    };
    send_packet(PKT_HEARTBEAT, 0, &hb, sizeof(hb));

    /* Watchdog: if no host heartbeat for >2s, blink rapidly */
    if (k_uptime_get_32() - last_heartbeat > 2000) {
        gpio_pin_toggle_dt(&status_led);
    }
}

/* ─── Workqueue Stack ─── */
K_THREAD_STACK_DEFINE(probe_work_q_stack, 4096);

/* ─── Main ─── */
int main(void) {
    LOG_INF("ExMachina Smart Bench Probe starting...");

    /* Initialize timer */
    k_timer_init(&hb_timer, heartbeat_timer, NULL);

    /* Initialize devices */
    i2c0_dev = DEVICE_DT_GET(I2C0_NODE);
    adc_dev = DEVICE_DT_GET(ADC_NODE);
    uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

    if (!device_is_ready(i2c0_dev) || !device_is_ready(adc_dev) || !device_is_ready(uart_dev)) {
        LOG_ERR("Devices not ready");
        return -ENODEV;
    }

    /* Status LED */
    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_ACTIVE);

    /* USB CDC-ACM */
    int ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("USB enable failed: %d", ret);
        return ret;
    }

    /* Set RX callback */
    uart_irq_callback_user_data_set(uart_dev, cdc_acm_rx_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    /* Initialize workqueue */
    k_work_queue_start(&probe_work_q, probe_work_q_stack,
                       K_THREAD_STACK_SIZEOF(probe_work_q_stack),
                       K_PRIO_COOP(7), NULL);
    k_thread_name_set(probe_work_q.thread, "probe_wq");

    /* Start heartbeat timer (500ms) */
    k_timer_start(&hb_timer, K_MSEC(500), K_MSEC(500));

    LOG_INF("Probe ready on USB CDC");

    /* Main loop */
    while (1) {
        k_sleep(K_MSEC(100));

        /* Blink status LED */
        gpio_pin_toggle_dt(&status_led);
    }
    return 0;
}