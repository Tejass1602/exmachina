#ifndef ZEPHYR_DRIVERS_ADC_H
#define ZEPHYR_DRIVERS_ADC_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_GAIN_1 0
#define ADC_REF_INTERNAL 0
#define ADC_ACQ_TIME_DEFAULT 0

struct adc_channel_cfg {
    uint8_t gain;
    uint8_t reference;
    uint16_t acquisition_time;
    uint8_t channel_id;
    uint8_t differential;
};

struct adc_sequence {
    uint16_t channels;
    void *buffer;
    size_t buffer_size;
    uint8_t resolution;
    uint8_t oversampling;
    bool calibrate;
};

int adc_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg);
int adc_read(const struct device *dev, const struct adc_sequence *seq);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_ADC_H */