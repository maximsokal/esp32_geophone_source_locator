#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int k_timeout_t;
#define K_NO_WAIT 0

struct k_msgq {
    size_t msg_size;
    unsigned max_msgs;
    unsigned head;
    unsigned tail;
    unsigned count;
    uint8_t storage[4][64];
};

#define K_MSGQ_DEFINE(name, msg_size_value, max_msgs_value, align_value) \
    struct k_msgq name = { .msg_size = (msg_size_value), .max_msgs = (max_msgs_value) }

static inline void k_msgq_purge(struct k_msgq *q)
{
    q->head = 0U;
    q->tail = 0U;
    q->count = 0U;
}

static inline int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout)
{
    (void)timeout;
    if (q->count >= q->max_msgs) {
        return -ENOMSG;
    }
    memcpy(q->storage[q->tail], data, q->msg_size);
    q->tail = (q->tail + 1U) % q->max_msgs;
    ++q->count;
    return 0;
}

static inline int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout)
{
    (void)timeout;
    if (q->count == 0U) {
        return -ENOMSG;
    }
    memcpy(data, q->storage[q->head], q->msg_size);
    q->head = (q->head + 1U) % q->max_msgs;
    --q->count;
    return 0;
}

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work {
    k_work_handler_t handler;
};

static inline void k_work_init(struct k_work *work, k_work_handler_t handler)
{
    work->handler = handler;
}

int k_work_submit(struct k_work *work);

int64_t k_uptime_get(void);
uint32_t k_cycle_get_32(void);

typedef unsigned int k_spinlock_key_t;
struct k_spinlock {
    unsigned int unused;
};

#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
void nrf_bootstrap_runtime_test_spinlock_hook(struct k_spinlock *lock);
void nrf_bootstrap_runtime_test_spinunlock_hook(struct k_spinlock *lock);
#endif

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
    nrf_bootstrap_runtime_test_spinlock_hook(lock);
#endif
    (void)lock;
    return 0U;
}

static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
    (void)lock;
    (void)key;
#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
    nrf_bootstrap_runtime_test_spinunlock_hook(lock);
#endif
}

#endif
