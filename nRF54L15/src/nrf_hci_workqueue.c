#include "nrf_hci_workqueue.h"

#include <errno.h>
#include <stdbool.h>

#define NRF_HCI_WORKQUEUE_STACK_SIZE 2048U
#define NRF_HCI_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(10)

/* shortcut: 2048 bytes matches the existing inventory, resize after target stack measurement. */
K_THREAD_STACK_DEFINE(s_hci_workqueue_stack, NRF_HCI_WORKQUEUE_STACK_SIZE);
static struct k_work_q s_hci_workqueue;
static bool s_hci_workqueue_started;
static const struct k_work_queue_config s_hci_workqueue_config = {
    .name = "cs_hci_workqueue",
};

void nrf_hci_workqueue_start(void)
{
    if (s_hci_workqueue_started) {
        return;
    }

    k_work_queue_init(&s_hci_workqueue);
    k_work_queue_start(
        &s_hci_workqueue,
        s_hci_workqueue_stack,
        K_THREAD_STACK_SIZEOF(s_hci_workqueue_stack),
        NRF_HCI_WORKQUEUE_PRIORITY,
        &s_hci_workqueue_config);
    s_hci_workqueue_started = true;
}

int nrf_hci_workqueue_submit(struct k_work *work)
{
    if (!s_hci_workqueue_started) {
        return -EAGAIN;
    }
    return k_work_submit_to_queue(&s_hci_workqueue, work);
}

int nrf_hci_workqueue_reschedule(struct k_work_delayable *work, k_timeout_t delay)
{
    if (!s_hci_workqueue_started) {
        return -EAGAIN;
    }
    return k_work_reschedule_for_queue(&s_hci_workqueue, work, delay);
}

#if defined(NRF_HCI_WORKQUEUE_TEST_HOOKS)
void nrf_hci_workqueue_test_forget_started(void)
{
    s_hci_workqueue_started = false;
}
#endif
