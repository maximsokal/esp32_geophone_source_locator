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

#define FAKE_QUEUE_BYTES 128U

typedef struct {
    size_t item_size;
    bool has_item;
    unsigned char item[FAKE_QUEUE_BYTES];
} fake_queue_t;

typedef struct { UBaseType_t count; } fake_sem_t;

static fake_queue_t s_queue;
static fake_sem_t s_sem;
static int s_task_token;
static bool s_fail_queue_create;
static bool s_fail_sem_create;
static bool s_fail_task_create;
static bool s_fail_queue_send;
static bool s_inject_stop_when_empty;
static bool s_release_callback_inflight_on_delay;
static unsigned s_barrier_delay_calls;
static unsigned s_queue_delete_calls;
static unsigned s_sem_delete_calls;
static unsigned s_process_info_calls;
static unsigned s_process_erase_calls;
static unsigned s_callback_calls;
static bool s_callback_saw_gate_held;
static bool s_callback_saw_inflight_accounted;
static TaskFunction_t s_task_entry;
static UBaseType_t s_task_priority;
static uint32_t s_task_stack;
static BaseType_t s_task_core;
static uint32_t s_task_caps;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length == 1U);
    assert(item_size <= sizeof(s_queue.item));
    if (s_fail_queue_create) return NULL;
    memset(&s_queue, 0, sizeof(s_queue));
    s_queue.item_size = item_size;
    return &s_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks)
{
    (void)ticks;
    assert(queue == &s_queue);
    if (s_fail_queue_send || s_queue.has_item) return pdFALSE;
    memcpy(s_queue.item, item, s_queue.item_size);
    s_queue.has_item = true;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    assert(queue == &s_queue);
    assert(ticks == portMAX_DELAY);
    if (s_queue.has_item) {
        memcpy(item, s_queue.item, s_queue.item_size);
        s_queue.has_item = false;
        return pdTRUE;
    }
    if (s_inject_stop_when_empty) {
        storage_command_t stop = STORAGE_COMMAND_STOP;
        memcpy(item, &stop, sizeof(stop));
        s_inject_stop_when_empty = false;
        return pdTRUE;
    }
    return pdFALSE;
}

void vQueueDelete(QueueHandle_t queue)
{
    assert(queue == &s_queue);
    ++s_queue_delete_calls;
    s_queue.has_item = false;
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
    if (ticks == 1U) {
        storage_service_callback_slot_t *slot =
            storage_service_internal_callback_slot();
        assert(s_release_callback_inflight_on_delay);
        assert(slot->in_flight == 1U);
        slot->in_flight = 0U;
        s_release_callback_inflight_on_delay = false;
        ++s_barrier_delay_calls;
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
    event->type = STORAGE_SERVICE_EVENT_ERASE_DONE;
    event->result = ESP_OK;
    return false;
}

static void on_event(const storage_service_event_t *event, void *context)
{
    storage_service_callback_slot_t *slot =
        storage_service_internal_callback_slot();

    assert(context == &s_callback_calls);
    assert(event->type == STORAGE_SERVICE_EVENT_INFO_DONE);
    assert(event->result == ESP_OK);
    assert(event->info.capacity_bytes == 1234U);
    ++s_callback_calls;
    s_callback_saw_gate_held = (s_sem.count == 0U);
    s_callback_saw_inflight_accounted = (slot->in_flight == 1U);
}

static void reset_failures(void)
{
    s_fail_queue_create = false;
    s_fail_sem_create = false;
    s_fail_task_create = false;
    s_fail_queue_send = false;
    s_inject_stop_when_empty = false;
    s_release_callback_inflight_on_delay = false;
    s_task_entry = NULL;
}

int main(void)
{
    storage_service_callback_slot_t *slot =
        storage_service_internal_callback_slot();

    assert(storage_service_set_event_callback(on_event, NULL) == ESP_ERR_INVALID_STATE);
    assert(storage_service_info_async() == ESP_ERR_INVALID_STATE);
    assert(storage_service_erase_data_async() == ESP_ERR_INVALID_STATE);
    assert(!storage_service_is_busy());
    storage_service_shutdown();

    s_fail_queue_create = true;
    assert(storage_service_init() == ESP_ERR_NO_MEM);
    reset_failures();

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

    /* Model a callback already snapped by the worker before unregister. */
    slot->in_flight = 1U;
    s_release_callback_inflight_on_delay = true;
    assert(storage_service_set_event_callback(NULL, NULL) == ESP_OK);
    assert(s_barrier_delay_calls == 1U);
    assert(slot->callback == NULL);
    assert(slot->context == NULL);
    assert(slot->in_flight == 0U);
    assert(storage_service_set_event_callback(on_event, &s_callback_calls) == ESP_OK);

    s_fail_queue_send = true;
    assert(storage_service_info_async() == ESP_ERR_TIMEOUT);
    assert(!storage_service_is_busy());
    assert(s_sem.count == 1U);
    s_fail_queue_send = false;

    assert(storage_service_info_async() == ESP_OK);
    assert(storage_service_is_busy());
    assert(storage_service_erase_data_async() == ESP_ERR_INVALID_STATE);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_info_calls == 1U);
    assert(s_callback_calls == 1U);
    assert(s_callback_saw_gate_held);
    assert(s_callback_saw_inflight_accounted);
    assert(slot->in_flight == 0U);
    assert(!storage_service_is_busy());
    assert(s_sem.count == 1U);

    storage_service_shutdown();
    assert(slot->callback == NULL);
    assert(slot->context == NULL);
    assert(slot->in_flight == 0U);
    assert(!storage_service_is_busy());
    assert(s_queue_delete_calls == 3U);
    assert(s_sem_delete_calls == 2U);

    assert(storage_service_init() == ESP_OK);
    assert(storage_service_erase_data_async() == ESP_OK);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_erase_calls == 1U);
    assert(s_callback_calls == 1U);
    storage_service_shutdown();

    assert(storage_service_init() == ESP_OK);
    storage_service_shutdown();
    assert(!storage_service_is_busy());

    return 0;
}
