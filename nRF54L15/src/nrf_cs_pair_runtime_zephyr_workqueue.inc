#include <errno.h>
#include <stdbool.h>

#define NRF_CS_WORKQUEUE_STACK_SIZE 2048U
#define NRF_CS_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(10)

/* shortcut: 2048 bytes matches system-workqueue budget, resize after target stack measurement. */
K_THREAD_STACK_DEFINE(s_cs_workqueue_stack, NRF_CS_WORKQUEUE_STACK_SIZE);
static struct k_work_q s_cs_workqueue;
static bool s_cs_workqueue_started;
static const struct k_work_queue_config s_cs_workqueue_config = {
    .name = "cs_hci_workqueue",
};

static void start_cs_workqueue(void)
{
    if (s_cs_workqueue_started) {
        return;
    }

    k_work_queue_init(&s_cs_workqueue);
    /* Match sensor_worker priority 10; TWIS and watchdog work stay on the system queue. */
    k_work_queue_start(
        &s_cs_workqueue,
        s_cs_workqueue_stack,
        K_THREAD_STACK_SIZEOF(s_cs_workqueue_stack),
        NRF_CS_WORKQUEUE_PRIORITY,
        &s_cs_workqueue_config);
    s_cs_workqueue_started = true;
}

static int submit_cs_work(struct k_work *work)
{
    if (!s_cs_workqueue_started) {
        return -EAGAIN;
    }
    return k_work_submit_to_queue(&s_cs_workqueue, work);
}

static int reschedule_cs_work(struct k_work_delayable *work, k_timeout_t delay)
{
    if (!s_cs_workqueue_started) {
        return -EAGAIN;
    }
    return k_work_reschedule_for_queue(&s_cs_workqueue, work, delay);
}
