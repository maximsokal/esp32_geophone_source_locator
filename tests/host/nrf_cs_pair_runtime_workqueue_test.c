#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "zephyr/kernel.h"
#include "nrf_cs_pair_runtime_zephyr_workqueue.inc"

static unsigned int s_hci_handler_calls;
static unsigned int s_system_handler_calls;

static void blocking_hci_work_handler(struct k_work *work)
{
    (void)work;
    s_hci_handler_calls++;
    host_kernel_block_current_queue();
}

static void system_work_handler(struct k_work *work)
{
    (void)work;
    s_system_handler_calls++;
}

/* fail-before(real): the real CS queue must own submits and timeout reschedules. */
static void test_cs_work_items_submit_and_reschedule_to_dedicated_queue(void)
{
    struct k_work work;
    struct k_work_delayable timeout_work;

    host_kernel_reset();
    k_work_init(&work, system_work_handler);
    k_work_init_delayable(&timeout_work, system_work_handler);
    start_cs_workqueue();
    start_cs_workqueue();
    assert(host_k_work_queue_init_count == 1U);
    assert(host_k_work_queue_start_count == 1U);
    assert(s_cs_workqueue.name != NULL);
    assert(strcmp(s_cs_workqueue.name, "cs_hci_workqueue") == 0);
    assert(s_cs_workqueue.stack_size == 2048U);
    assert(s_cs_workqueue.priority == 10);

    assert(submit_cs_work(&work) == 1);
    assert(work.queue == &s_cs_workqueue);
    assert(host_first_submit_order > host_k_work_queue_start_order);

    assert(reschedule_cs_work(&timeout_work, K_MSEC(17)) == 1);
    assert(timeout_work.work.queue == &s_cs_workqueue);
    assert(timeout_work.delay == 17);
    assert(host_k_work_reschedule_count == 1U);
    assert(host_workqueue_run_one(&s_cs_workqueue));
}

/* fail-before(real): reject a CS submit when the runtime has not started its queue. */
static void test_unstarted_queue_rejects_submit(void)
{
    struct k_work work;

    host_kernel_reset();
    k_work_init(&work, system_work_handler);
    assert(submit_cs_work(&work) == -EAGAIN);
    assert(host_k_work_submit_count == 0U);
}

/* mutation-only: this queue stub models a non-returning CS handler by blocking its queue. */
static void test_blocked_cs_queue_does_not_delay_system_items(void)
{
    struct k_work cs_work;
    struct k_work wdt_probe_work;
    struct k_work bootstrap_assign_work;
    struct k_work bootstrap_cs_execute_work;

    host_kernel_reset();
    s_hci_handler_calls = 0U;
    s_system_handler_calls = 0U;
    start_cs_workqueue();
    k_work_init(&cs_work, blocking_hci_work_handler);
    k_work_init(&wdt_probe_work, system_work_handler);
    k_work_init(&bootstrap_assign_work, system_work_handler);
    k_work_init(&bootstrap_cs_execute_work, system_work_handler);

    assert(submit_cs_work(&cs_work) == 1);
    assert(host_workqueue_run_one(&s_cs_workqueue));
    assert(s_cs_workqueue.blocked);
    assert(s_hci_handler_calls == 1U);

    assert(k_work_submit(&wdt_probe_work) == 1);
    assert(k_work_submit(&bootstrap_assign_work) == 1);
    assert(k_work_submit(&bootstrap_cs_execute_work) == 1);
    assert(wdt_probe_work.queue == &k_sys_work_q);
    assert(bootstrap_assign_work.queue == &k_sys_work_q);
    assert(bootstrap_cs_execute_work.queue == &k_sys_work_q);

    assert(host_workqueue_run_one(&k_sys_work_q));
    assert(host_workqueue_run_one(&k_sys_work_q));
    assert(host_workqueue_run_one(&k_sys_work_q));
    assert(!host_workqueue_run_one(&s_cs_workqueue));
    assert(s_system_handler_calls == 3U);
}

int main(void)
{
    test_unstarted_queue_rejects_submit();
    test_cs_work_items_submit_and_reschedule_to_dedicated_queue();
    test_blocked_cs_queue_does_not_delay_system_items();
    return 0;
}
