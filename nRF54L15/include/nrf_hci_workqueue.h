#ifndef NRF_HCI_WORKQUEUE_H
#define NRF_HCI_WORKQUEUE_H

#include <zephyr/kernel.h>

void nrf_hci_workqueue_start(void);
int nrf_hci_workqueue_submit(struct k_work *work);
int nrf_hci_workqueue_reschedule(struct k_work_delayable *work, k_timeout_t delay);

#if defined(NRF_HCI_WORKQUEUE_TEST_HOOKS)
void nrf_hci_workqueue_test_forget_started(void);
#endif

#endif
