#include "nrf_hci_workqueue.h"

static void start_cs_workqueue(void)
{
    nrf_hci_workqueue_start();
}

static int submit_cs_work(struct k_work *work)
{
    return nrf_hci_workqueue_submit(work);
}

static int reschedule_cs_work(struct k_work_delayable *work, k_timeout_t delay)
{
    return nrf_hci_workqueue_reschedule(work, delay);
}
