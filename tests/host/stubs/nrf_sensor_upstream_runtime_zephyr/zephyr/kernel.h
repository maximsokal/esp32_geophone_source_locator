#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H
#include <stddef.h>
#include <stdint.h>
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(x) ((k_timeout_t)(x))
#define K_PRIO_PREEMPT(x) (x)
typedef int64_t k_timeout_t;
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
void k_msgq_purge(struct k_msgq *q);
int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);
typedef unsigned int k_spinlock_key_t;
struct k_spinlock { unsigned unused; };
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) { (void)lock; return 0U; }
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) { (void)lock; (void)key; }
struct k_thread { int unused; };
typedef struct k_thread *k_tid_t;
#define K_THREAD_STACK_DEFINE(name, size) unsigned char name[(size)]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
k_tid_t k_thread_create(struct k_thread *thread, void *stack, size_t stack_size,
                        void (*entry)(void *, void *, void *),
                        void *p1, void *p2, void *p3,
                        int prio, uint32_t options, k_timeout_t delay);
int64_t k_uptime_get(void);
int32_t k_msleep(int32_t milliseconds);
#endif
