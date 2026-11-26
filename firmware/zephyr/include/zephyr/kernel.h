#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t k_timeout_t;
#define K_FOREVER ((k_timeout_t)0xFFFFFFFF)
#define K_MSEC(ms) ((k_timeout_t)(ms))
#define K_NO_WAIT ((k_timeout_t)0)
#define SYS_FOREVER_US 0xFFFFFFFF

#define K_PRIO_COOP(x) (-x)
#define K_PRIO_PREEMPT(x) (x)

typedef struct k_timer {
    void *timer;
} k_timer_t;

typedef void (*k_timer_expiry_t)(struct k_timer *timer);

void k_timer_init(struct k_timer *timer, k_timer_expiry_t expiry, void *stop);
void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period);
void k_timer_stop(struct k_timer *timer);
uint32_t k_uptime_get_32(void);
void k_sleep(k_timeout_t timeout);
void k_usleep(uint32_t us);
void k_msleep(uint32_t ms);

#define K_THREAD_STACK_DEFINE(name, size) static char name[size]
#define K_THREAD_STACK_SIZEOF(name) (sizeof(name))

#define BIT(x) (1 << (x))

/* DT macros for devicetree */
#define DT_ALIAS(name) name
#define DT_NODELABEL(name) name
#define DT_CHOSEN(name) name
#define DT_NODE_HAS_STATUS(node, status) 1
#define GPIO_DT_SPEC_GET(node, prop) {0, 0, 0}

typedef struct k_work_q {
    void *q;
    void *thread;
} k_work_q_t;

void k_work_queue_start(k_work_q_t *q, void *stack, size_t size, int prio, void *opts);
void k_thread_name_set(void *thread, const char *name);

#define K_TIMER_DEFINE(name, expiry, stop) \
    struct k_timer name; \
    static void name##_init(void) { k_timer_init(&name, expiry, stop); }

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_KERNEL_H */