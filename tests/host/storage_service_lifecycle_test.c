#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "storage_service.h"
#include "storage_service_internal.h"
#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "host_threads.h"

#define FAKE_QUEUE_BYTES 128U

typedef struct {
    size_t item_size;
    bool has_item;
    unsigned char item[FAKE_QUEUE_BYTES];
} fake_queue_t;

typedef struct { UBaseType_t count; } fake_sem_t;

_Thread_local unsigned fake_critical_depth;

static fake_queue_t s_queue;
static fake_sem_t s_sem;
static int s_task_token;
static bool s_fail_queue_create;
static bool s_fail_sem_create;
static bool s_fail_task_create;
static bool s_fail_queue_send;
static bool s_fail_queue_receive_once;
static bool s_queue_live;
static bool s_pause_next_queue_send;
static bool s_queue_send_paused;
static bool s_release_queue_send;
static bool s_hold_submit_wait;
static bool s_shutdown_waiting;
static bool s_shutdown_done;
static bool s_release_submit_wait;
static bool s_stale_queue_send;
static bool s_inject_stop_when_empty;
static bool s_release_callback_inflight_on_delay;
static bool s_release_content_inflight_on_delay;
static bool s_erase_succeeds = true;
static unsigned s_barrier_delay_calls;
static unsigned s_queue_delete_calls;
static unsigned s_sem_delete_calls;
static unsigned s_process_info_calls;
static unsigned s_process_erase_calls;
static unsigned s_callback_calls;
static unsigned s_content_changed_calls;
static bool s_callback_saw_gate_held;
static bool s_callback_saw_inflight_accounted;
static bool s_content_saw_gate_held;
static bool s_content_saw_inflight_accounted;
static TaskFunction_t s_task_entry;
static UBaseType_t s_task_priority;
static uint32_t s_task_stack;
static BaseType_t s_task_core;
static uint32_t s_task_caps;
static TickType_t s_last_queue_send_ticks;
static host_mutex_t s_race_lock = HOST_MUTEX_INITIALIZER;
static host_cond_t s_race_changed = HOST_COND_INITIALIZER;
static esp_err_t s_race_submit_result;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length == 1U);
    assert(item_size <= sizeof(s_queue.item));
    if (s_fail_queue_create) return NULL;
    memset(&s_queue, 0, sizeof(s_queue));
    s_queue.item_size = item_size;
    s_queue_live = true;
    return &s_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    s_last_queue_send_ticks = ticks;
    assert(queue == &s_queue);
    assert(host_mutex_lock(&s_race_lock) == 0);
    if (s_pause_next_queue_send) {
        s_pause_next_queue_send = false;
        s_queue_send_paused = true;
        assert(host_cond_broadcast(&s_race_changed) == 0);
        while (!s_release_queue_send) {
            assert(host_cond_wait_for(&s_race_changed, &s_race_lock, 5000U) == 0);
        }
    }
    if (!s_queue_live) {
        s_stale_queue_send = true;
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdFALSE;
    }
    if (s_fail_queue_send) {
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdFALSE;
    }
    while (s_queue.has_item && ticks != 0U) {
        const uint32_t wait_ms = ticks == portMAX_DELAY ? 5000U : (uint32_t)ticks;
        const int wait_result = host_cond_wait_for(
            &s_race_changed,
            &s_race_lock,
            wait_ms
        );
        if (ticks != portMAX_DELAY && wait_result != 0) {
            assert(host_mutex_unlock(&s_race_lock) == 0);
            return pdFALSE;
        }
    }
    if (s_queue.has_item) {
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdFALSE;
    }
    memcpy(s_queue.item, item, s_queue.item_size);
    s_queue.has_item = true;
    assert(host_cond_broadcast(&s_race_changed) == 0);
    assert(host_mutex_unlock(&s_race_lock) == 0);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    assert(queue == &s_queue);
    assert(ticks == portMAX_DELAY);
    assert(host_mutex_lock(&s_race_lock) == 0);
    if (s_fail_queue_receive_once) {
        const storage_command_t stale = STORAGE_COMMAND_INFO;
        memcpy(item, &stale, sizeof(stale));
        s_fail_queue_receive_once = false;
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdFALSE;
    }
    if (s_queue.has_item) {
        memcpy(item, s_queue.item, s_queue.item_size);
        s_queue.has_item = false;
        assert(host_cond_broadcast(&s_race_changed) == 0);
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdTRUE;
    }
    if (s_inject_stop_when_empty) {
        storage_command_t stop = STORAGE_COMMAND_STOP;
        memcpy(item, &stop, sizeof(stop));
        s_inject_stop_when_empty = false;
        assert(host_mutex_unlock(&s_race_lock) == 0);
        return pdTRUE;
    }
    assert(host_mutex_unlock(&s_race_lock) == 0);
    return pdFALSE;
}

void vQueueDelete(QueueHandle_t queue)
{
    assert(queue == &s_queue);
    assert(host_mutex_lock(&s_race_lock) == 0);
    ++s_queue_delete_calls;
    s_queue_live = false;
    s_queue.has_item = false;
    assert(host_cond_broadcast(&s_race_changed) == 0);
    assert(host_mutex_unlock(&s_race_lock) == 0);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (s_fail_sem_create) return NULL;
    s_sem.count = 0;
    return &s_sem;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &s_sem);
    assert(ticks == 0);
    if (s_sem.count == 0) return pdFALSE;
    s_sem.count = 0;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_sem);
    s_sem.count = 1;
    return pdTRUE;
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_sem);
    return s_sem.count;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_sem);
    ++s_sem_delete_calls;
    s_sem.count = 0;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core, uint32_t caps)
{
    (void)arg;
    assert(strcmp(name, "storage_service") == 0);
    if (s_fail_task_create) return pdFALSE;
    s_task_entry = task;
    s_task_stack = stack;
    s_task_priority = priority;
    s_task_core = core;
    s_task_caps = caps;
    *handle = &s_task_token;
    return pdPASS;
}

void vTaskDeleteWithCaps(TaskHandle_t task) { assert(task == NULL); }

void vTaskDelay(TickType_t ticks)
{
    assert(fake_critical_depth == 0U);

    if (ticks == 1U) {
        assert(host_mutex_lock(&s_race_lock) == 0);
        if (s_hold_submit_wait) {
            s_shutdown_waiting = true;
            assert(host_cond_broadcast(&s_race_changed) == 0);
            while (!s_release_submit_wait) {
                assert(host_cond_wait_for(&s_race_changed, &s_race_lock, 5000U) == 0);
            }
            s_hold_submit_wait = false;
        }
        assert(host_mutex_unlock(&s_race_lock) == 0);
        if (s_release_callback_inflight_on_delay) {
            storage_service_callback_slot_t *slot =
                storage_service_internal_callback_slot();
            assert(slot->in_flight == 1U);
            slot->in_flight = 0U;
            s_release_callback_inflight_on_delay = false;
            ++s_barrier_delay_calls;
            return;
        }
        if (s_release_content_inflight_on_delay) {
            storage_service_content_observer_slot_t *slot =
                storage_service_internal_content_observer_slot();
            assert(slot->in_flight == 1U);
            slot->in_flight = 0U;
            s_release_content_inflight_on_delay = false;
            ++s_barrier_delay_calls;
            return;
        }
        return;
    }

    assert(ticks == pdMS_TO_TICKS(10));
    if (s_task_entry != NULL) {
        TaskFunction_t task = s_task_entry;
        s_task_entry = NULL;
        task(NULL);
    }
}

bool storage_service_process_info(storage_service_event_t *event)
{
    ++s_process_info_calls;
    event->type = STORAGE_SERVICE_EVENT_INFO_DONE;
    event->result = ESP_OK;
    event->info.capacity_bytes = 1234U;
    return true;
}

bool storage_service_process_erase(storage_service_event_t *event)
{
    ++s_process_erase_calls;
    event->type = s_erase_succeeds
        ? STORAGE_SERVICE_EVENT_ERASE_DONE
        : STORAGE_SERVICE_EVENT_ERASE_FAILED;
    event->result = s_erase_succeeds ? ESP_OK : ESP_FAIL;
    /* Model shutdown-pending success: TFT redraw is suppressed. */
    return !s_erase_succeeds;
}

static void on_event(const storage_service_event_t *event, void *context)
{
    storage_service_callback_slot_t *slot =
        storage_service_internal_callback_slot();

    assert(fake_critical_depth == 0U);
    assert(context == &s_callback_calls);
    assert(event->type == STORAGE_SERVICE_EVENT_INFO_DONE);
    assert(event->result == ESP_OK);
    assert(event->info.capacity_bytes == 1234U);
    ++s_callback_calls;
    s_callback_saw_gate_held = (s_sem.count == 0U);
    s_callback_saw_inflight_accounted = (slot->in_flight == 1U);
}

static void on_content_changed(void *context)
{
    storage_service_content_observer_slot_t *slot =
        storage_service_internal_content_observer_slot();

    assert(fake_critical_depth == 0U);
    assert(context == &s_content_changed_calls);
    ++s_content_changed_calls;
    s_content_saw_gate_held = (s_sem.count == 0U);
    s_content_saw_inflight_accounted = (slot->in_flight == 1U);
}

static void reset_failures(void)
{
    s_fail_queue_create = false;
    s_fail_sem_create = false;
    s_fail_task_create = false;
    s_fail_queue_send = false;
    s_fail_queue_receive_once = false;
    s_inject_stop_when_empty = false;
    s_release_callback_inflight_on_delay = false;
    s_release_content_inflight_on_delay = false;
    s_task_entry = NULL;
}


static void *race_submit_info(void *unused)
{
    (void)unused;
    s_race_submit_result = storage_service_info_async();
    return NULL;
}


static void *race_shutdown_service(void *unused)
{
    (void)unused;
    storage_service_shutdown();
    assert(host_mutex_lock(&s_race_lock) == 0);
    s_shutdown_done = true;
    assert(host_cond_broadcast(&s_race_changed) == 0);
    assert(host_mutex_unlock(&s_race_lock) == 0);
    return NULL;
}


static void test_submit_finishes_before_storage_queue_teardown(void)
{
    assert(storage_service_init() == ESP_OK);
    assert(s_queue.item_size == sizeof(storage_command_t));
    storage_command_t untouched = STORAGE_COMMAND_ERASE_DATA;
    assert(xQueueReceive(&s_queue, &untouched, portMAX_DELAY) == pdFALSE);
    assert(untouched == STORAGE_COMMAND_ERASE_DATA);

    assert(host_mutex_lock(&s_race_lock) == 0);
    s_pause_next_queue_send = true;
    s_queue_send_paused = false;
    s_release_queue_send = false;
    s_hold_submit_wait = true;
    s_shutdown_waiting = false;
    s_shutdown_done = false;
    s_release_submit_wait = false;
    assert(host_mutex_unlock(&s_race_lock) == 0);

    host_thread_t producer;
    host_thread_t shutdown;
    assert(host_thread_create(&producer, race_submit_info, NULL) == 0);
    assert(host_mutex_lock(&s_race_lock) == 0);
    while (!s_queue_send_paused) {
        assert(host_cond_wait_for(&s_race_changed, &s_race_lock, 5000U) == 0);
    }
    assert(host_mutex_unlock(&s_race_lock) == 0);

    const unsigned deletes_before = s_queue_delete_calls;
    const unsigned sem_deletes_before = s_sem_delete_calls;
    s_inject_stop_when_empty = true;
    assert(host_thread_create(&shutdown, race_shutdown_service, NULL) == 0);
    assert(host_mutex_lock(&s_race_lock) == 0);
    while (!s_shutdown_waiting && !s_shutdown_done) {
        assert(host_cond_wait_for(&s_race_changed, &s_race_lock, 5000U) == 0);
    }
    const bool shutdown_before_submit_end = s_shutdown_done;
    assert(host_mutex_unlock(&s_race_lock) == 0);
    assert(!shutdown_before_submit_end);
    assert(s_queue_delete_calls == deletes_before);

    assert(host_mutex_lock(&s_race_lock) == 0);
    s_release_queue_send = true;
    assert(host_cond_broadcast(&s_race_changed) == 0);
    assert(host_mutex_unlock(&s_race_lock) == 0);
    assert(host_thread_join(producer) == 0);

    assert(s_race_submit_result == ESP_OK);
    assert(s_queue_live && s_queue.has_item);
    storage_command_t queued;
    memcpy(&queued, s_queue.item, sizeof(queued));
    assert(queued == STORAGE_COMMAND_INFO);
    TaskFunction_t worker = s_task_entry;
    s_task_entry = NULL;
    worker(NULL);
    assert(s_sem.count == 1U);

    assert(host_mutex_lock(&s_race_lock) == 0);
    s_release_submit_wait = true;
    assert(host_cond_broadcast(&s_race_changed) == 0);
    assert(host_mutex_unlock(&s_race_lock) == 0);
    assert(host_thread_join(shutdown) == 0);

    assert(!s_stale_queue_send);
    assert(s_last_queue_send_ticks == portMAX_DELAY);
    assert(!s_queue_live);
    assert(s_queue_delete_calls == deletes_before + 1U);
    assert(s_sem_delete_calls == sem_deletes_before + 1U);
    assert(!s_stale_queue_send);
    assert(storage_service_info_async() == ESP_ERR_INVALID_STATE);

    assert(storage_service_init() == ESP_OK);
    storage_service_shutdown();
    assert(!s_queue_live);
    assert(s_queue_delete_calls == deletes_before + 2U);
    assert(s_sem_delete_calls == sem_deletes_before + 2U);
    assert(!s_stale_queue_send);
}


static void test_shutdown_send_failure_retains_queue(void)
{
    const unsigned deletes_before = s_queue_delete_calls;
    const unsigned sem_deletes_before = s_sem_delete_calls;
    assert(storage_service_init() == ESP_OK);
    s_fail_queue_send = true;
    s_inject_stop_when_empty = true;
    storage_service_shutdown();
    assert(s_last_queue_send_ticks == portMAX_DELAY);
    assert(s_queue_live);
    assert(s_queue_delete_calls == deletes_before);
    assert(s_sem_delete_calls == sem_deletes_before);

    s_fail_queue_send = false;
    storage_service_shutdown();
    assert(!s_queue_live);
    assert(s_queue_delete_calls == deletes_before + 1U);
    assert(s_sem_delete_calls == sem_deletes_before + 1U);
    s_inject_stop_when_empty = false;
}


static void test_submit_failure_returns_timeout_and_releases_gate(void)
{
    s_fail_queue_send = true;
    assert(storage_service_info_async() == ESP_ERR_TIMEOUT);
    assert(s_last_queue_send_ticks == 0U);
    assert(!storage_service_is_busy());
    assert(s_sem.count == 1U);
    s_fail_queue_send = false;
}


static void test_queue_creation_failure_returns_no_memory(void)
{
    const unsigned queues_deleted_before = s_queue_delete_calls;
    const unsigned semaphores_deleted_before = s_sem_delete_calls;
    s_fail_queue_create = true;
    assert(storage_service_init() == ESP_ERR_NO_MEM);
    reset_failures();
    assert(s_queue_delete_calls == queues_deleted_before);
    assert(s_sem_delete_calls == semaphores_deleted_before);
    assert(!storage_service_is_busy());
}


static void test_receive_timeout_ignores_stale_output(void)
{
    const unsigned info_before = s_process_info_calls;
    const unsigned erase_before = s_process_erase_calls;
    assert(storage_service_init() == ESP_OK);
    s_fail_queue_receive_once = true;
    s_inject_stop_when_empty = true;
    TaskFunction_t worker = s_task_entry;
    s_task_entry = NULL;
    worker(NULL);
    assert(s_process_info_calls == info_before);
    assert(s_process_erase_calls == erase_before);
    storage_service_shutdown();
    assert(!s_queue_live);
}

int main(void)
{
    storage_service_callback_slot_t *slot =
        storage_service_internal_callback_slot();
    storage_service_content_observer_slot_t *content_slot =
        storage_service_internal_content_observer_slot();

    assert(storage_service_set_event_callback(on_event, NULL) == ESP_ERR_INVALID_STATE);
    assert(storage_service_set_content_changed_observer(
        on_content_changed,
        &s_content_changed_calls
    ) == ESP_OK);
    assert(content_slot->callback == on_content_changed);
    assert(storage_service_info_async() == ESP_ERR_INVALID_STATE);
    assert(storage_service_erase_data_async() == ESP_ERR_INVALID_STATE);
    assert(!storage_service_is_busy());
    storage_service_shutdown();
    assert(content_slot->callback == NULL);
    assert(content_slot->context == NULL);
    assert(content_slot->in_flight == 0U);

    test_queue_creation_failure_returns_no_memory();

    s_fail_sem_create = true;
    assert(storage_service_init() == ESP_ERR_NO_MEM);
    assert(s_queue_delete_calls == 1U);
    reset_failures();

    s_fail_task_create = true;
    assert(storage_service_init() == ESP_ERR_NO_MEM);
    assert(s_queue_delete_calls == 2U);
    assert(s_sem_delete_calls == 1U);
    reset_failures();

    assert(storage_service_init() == ESP_OK);
    assert(storage_service_init() == ESP_OK);
    assert(s_task_entry != NULL);
    assert(s_task_stack == APP_TASK_STACK_STORAGE_SERVICE);
    assert(s_task_priority == APP_TASK_PRIORITY_STORAGE_SERVICE);
    assert(s_task_core == APP_TASK_CORE_SERVICES);
    assert(s_task_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(storage_service_set_event_callback(on_event, &s_callback_calls) == ESP_OK);
    assert(storage_service_set_content_changed_observer(
        on_content_changed,
        &s_content_changed_calls
    ) == ESP_OK);

    /* Model callbacks already snapped before unregister. Both slots must drain
     * independently without holding their SMP spinlocks across vTaskDelay. */
    slot->in_flight = 1U;
    s_release_callback_inflight_on_delay = true;
    assert(storage_service_set_event_callback(NULL, NULL) == ESP_OK);
    assert(s_barrier_delay_calls == 1U);
    assert(slot->callback == NULL);
    assert(slot->context == NULL);
    assert(slot->in_flight == 0U);
    assert(storage_service_set_event_callback(on_event, &s_callback_calls) == ESP_OK);

    content_slot->in_flight = 1U;
    s_release_content_inflight_on_delay = true;
    assert(storage_service_set_content_changed_observer(NULL, NULL) == ESP_OK);
    assert(s_barrier_delay_calls == 2U);
    assert(content_slot->callback == NULL);
    assert(content_slot->context == NULL);
    assert(content_slot->in_flight == 0U);
    assert(storage_service_set_content_changed_observer(
        on_content_changed,
        &s_content_changed_calls
    ) == ESP_OK);

    test_submit_failure_returns_timeout_and_releases_gate();

    assert(storage_service_info_async() == ESP_OK);
    assert(storage_service_is_busy());
    assert(storage_service_erase_data_async() == ESP_ERR_INVALID_STATE);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_info_calls == 1U);
    assert(s_callback_calls == 1U);
    assert(s_content_changed_calls == 0U);
    assert(s_callback_saw_gate_held);
    assert(s_callback_saw_inflight_accounted);
    assert(slot->in_flight == 0U);
    assert(!storage_service_is_busy());
    assert(s_sem.count == 1U);

    storage_service_shutdown();
    assert(s_last_queue_send_ticks == portMAX_DELAY);
    assert(slot->callback == NULL);
    assert(slot->context == NULL);
    assert(slot->in_flight == 0U);
    assert(content_slot->callback == NULL);
    assert(content_slot->context == NULL);
    assert(content_slot->in_flight == 0U);
    assert(!storage_service_is_busy());
    assert(s_queue_delete_calls == 3U);
    assert(s_sem_delete_calls == 2U);

    /* Successful ERASE publishes content change even when process_erase()
     * suppresses the TFT redraw (notify=false), e.g. shutdown pending. */
    s_erase_succeeds = true;
    assert(storage_service_init() == ESP_OK);
    assert(storage_service_set_content_changed_observer(
        on_content_changed,
        &s_content_changed_calls
    ) == ESP_OK);
    assert(storage_service_erase_data_async() == ESP_OK);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_erase_calls == 1U);
    assert(s_callback_calls == 1U);
    assert(s_content_changed_calls == 1U);
    assert(s_content_saw_gate_held);
    assert(s_content_saw_inflight_accounted);
    assert(content_slot->in_flight == 0U);
    storage_service_shutdown();

    /* Failed ERASE must not advertise an empty authoritative store. */
    s_erase_succeeds = false;
    assert(storage_service_init() == ESP_OK);
    assert(storage_service_set_content_changed_observer(
        on_content_changed,
        &s_content_changed_calls
    ) == ESP_OK);
    assert(storage_service_erase_data_async() == ESP_OK);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_erase_calls == 2U);
    assert(s_content_changed_calls == 1U);
    storage_service_shutdown();

    assert(storage_service_init() == ESP_OK);
    storage_service_shutdown();
    test_submit_finishes_before_storage_queue_teardown();
    test_shutdown_send_failure_retains_queue();
    test_receive_timeout_ignores_stale_output();
    assert(!storage_service_is_busy());
    assert(fake_critical_depth == 0U);

    return 0;
}
