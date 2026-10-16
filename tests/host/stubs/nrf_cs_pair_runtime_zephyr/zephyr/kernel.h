#ifndef NRF_CS_PAIR_RUNTIME_ZEPHYR_TEST_KERNEL_H
#define NRF_CS_PAIR_RUNTIME_ZEPHYR_TEST_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t k_timeout_t;
struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work_q;

struct k_work {
    k_work_handler_t handler;
    struct k_work_q *queue;
    bool queued;
};

struct k_work_delayable {
    struct k_work work;
    bool scheduled;
    k_timeout_t delay;
};

#define NRF_CS_HOST_WORK_CAPACITY 16U

struct k_work_q {
    bool started;
    bool blocked;
    int priority;
    const char *name;
    size_t stack_size;
    struct k_work *pending[NRF_CS_HOST_WORK_CAPACITY];
    size_t pending_count;
};

struct k_work_queue_config {
    const char *name;
    bool no_yield;
    bool essential;
    uint32_t work_timeout_ms;
};

extern struct k_work_q k_sys_work_q;
extern unsigned int host_k_work_queue_init_count;
extern unsigned int host_k_work_queue_start_count;
extern unsigned int host_k_work_submit_count;
extern unsigned int host_k_work_reschedule_count;
extern unsigned int host_k_work_queue_start_order;
extern unsigned int host_first_submit_order;
extern unsigned int host_work_order;

#define K_THREAD_STACK_DEFINE(name, size) static unsigned char name[(size)]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
#define K_PRIO_PREEMPT(priority) (priority)
#define K_MSEC(milliseconds) ((k_timeout_t)(milliseconds))

void host_kernel_reset(void);
bool host_workqueue_run_one(struct k_work_q *queue);
void host_kernel_block_current_queue(void);

void k_work_queue_init(struct k_work_q *queue);
void k_work_queue_start(
    struct k_work_q *queue,
    unsigned char *stack,
    size_t stack_size,
    int priority,
    const struct k_work_queue_config *config);
void k_work_init(struct k_work *work, k_work_handler_t handler);
void k_work_init_delayable(struct k_work_delayable *work, k_work_handler_t handler);
int k_work_submit(struct k_work *work);
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work);
int k_work_schedule_for_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay);
int k_work_reschedule_for_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay);

#endif
