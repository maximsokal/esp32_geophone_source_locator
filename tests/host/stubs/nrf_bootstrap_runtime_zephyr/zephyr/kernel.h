#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int32_t k_timeout_t;
#define K_NO_WAIT 0
#define K_THREAD_STACK_DEFINE(name, size) static unsigned char name[(size)]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
#define K_PRIO_PREEMPT(priority) (priority)

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
struct k_work_q {
    bool started;
    int priority;
    const char *name;
    size_t stack_size;
};
struct k_work_queue_config { const char *name; };
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work {
    k_work_handler_t handler;
    struct k_work_q *queue;
    bool queued;
};
struct k_work_delayable { struct k_work work; };
#define K_MSEC(ms) ((k_timeout_t)(ms))
static inline void k_work_init(struct k_work *work, k_work_handler_t handler)
{
    work->handler = handler;
    work->queue = NULL;
    work->queued = false;
}
static inline void k_work_init_delayable(struct k_work_delayable *work, k_work_handler_t handler)
{
    k_work_init(&work->work, handler);
}

extern struct k_work_q k_sys_work_q;
void k_work_queue_init(struct k_work_q *queue);
void k_work_queue_start(struct k_work_q *queue, unsigned char *stack, size_t stack_size, int priority, const struct k_work_queue_config *config);
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work);
int k_work_submit(struct k_work *work);
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work, k_timeout_t delay);

int64_t k_uptime_get(void);
uint32_t k_cycle_get_32(void);
static inline uint32_t k_cyc_to_us_floor32(uint32_t cycles) { return cycles; }

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
