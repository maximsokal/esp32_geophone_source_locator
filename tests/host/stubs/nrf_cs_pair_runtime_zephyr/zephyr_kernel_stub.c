#include <errno.h>
#include <string.h>

#include "zephyr/kernel.h"

struct k_work_q k_sys_work_q;
unsigned int host_k_work_queue_init_count;
unsigned int host_k_work_queue_start_count;
unsigned int host_k_work_submit_count;
unsigned int host_k_work_reschedule_count;
unsigned int host_k_work_queue_start_order;
unsigned int host_first_submit_order;
unsigned int host_work_order;

static struct k_work_q *s_current_queue;

void host_kernel_reset(void)
{
    memset(&k_sys_work_q, 0, sizeof(k_sys_work_q));
    k_sys_work_q.started = true;
    host_k_work_queue_init_count = 0U;
    host_k_work_queue_start_count = 0U;
    host_k_work_submit_count = 0U;
    host_k_work_reschedule_count = 0U;
    host_k_work_queue_start_order = 0U;
    host_first_submit_order = 0U;
    host_work_order = 0U;
    s_current_queue = NULL;
}

void k_work_queue_init(struct k_work_q *queue)
{
    memset(queue, 0, sizeof(*queue));
    host_k_work_queue_init_count++;
}

void k_work_queue_start(
    struct k_work_q *queue,
    unsigned char *stack,
    size_t stack_size,
    int priority,
    const struct k_work_queue_config *config)
{
    (void)stack;
    queue->started = true;
    queue->stack_size = stack_size;
    queue->priority = priority;
    queue->name = config != NULL ? config->name : NULL;
    host_k_work_queue_start_count++;
    host_k_work_queue_start_order = ++host_work_order;
}

void k_work_init(struct k_work *work, k_work_handler_t handler)
{
    memset(work, 0, sizeof(*work));
    work->handler = handler;
}

void k_work_init_delayable(struct k_work_delayable *work, k_work_handler_t handler)
{
    memset(work, 0, sizeof(*work));
    work->work.handler = handler;
}

int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work)
{
    if (queue == NULL || !queue->started || work == NULL) {
        return -EAGAIN;
    }
    if (work->queued) {
        return 0;
    }
    if (queue->pending_count >= NRF_CS_HOST_WORK_CAPACITY) {
        return -ENOMEM;
    }

    work->queue = queue;
    work->queued = true;
    queue->pending[queue->pending_count++] = work;
    host_k_work_submit_count++;
    if (host_first_submit_order == 0U) {
        host_first_submit_order = ++host_work_order;
    }
    return 1;
}

int k_work_submit(struct k_work *work)
{
    return k_work_submit_to_queue(&k_sys_work_q, work);
}

static int schedule_on_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay)
{
    if (queue == NULL || !queue->started || work == NULL) {
        return -EAGAIN;
    }
    work->work.queue = queue;
    work->scheduled = true;
    work->delay = delay;
    host_k_work_reschedule_count++;
    if (host_first_submit_order == 0U) {
        host_first_submit_order = ++host_work_order;
    }
    return 1;
}

int k_work_schedule_for_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay)
{
    return schedule_on_queue(queue, work, delay);
}

int k_work_reschedule_for_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay)
{
    return schedule_on_queue(queue, work, delay);
}

bool host_workqueue_run_one(struct k_work_q *queue)
{
    struct k_work *work;

    if (queue == NULL || queue->blocked || queue->pending_count == 0U) {
        return false;
    }

    work = queue->pending[0];
    memmove(&queue->pending[0], &queue->pending[1],
            (queue->pending_count - 1U) * sizeof(queue->pending[0]));
    queue->pending_count--;
    work->queued = false;
    s_current_queue = queue;
    work->handler(work);
    s_current_queue = NULL;
    return true;
}

void host_kernel_block_current_queue(void)
{
    if (s_current_queue != NULL) {
        s_current_queue->blocked = true;
    }
}
