#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "object_service.h"
#include "object_service_internal.h"
#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#define FAKE_QUEUE_BYTES 256U

typedef struct {
    size_t item_size;
    bool has_item;
    unsigned char item[FAKE_QUEUE_BYTES];
} fake_queue_t;

static fake_queue_t s_queue;
static int s_task_token;
static bool s_fail_queue_create;
static bool s_fail_task_create;
static bool s_fail_queue_send;
static bool s_inject_stop_when_empty;
static unsigned s_queue_delete_calls;
static unsigned s_process_calls;
static bool s_process_saw_busy;
static object_service_command_type_t s_last_processed_type;
static char s_last_processed_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
static TaskFunction_t s_task_entry;
static uint32_t s_task_stack;
static UBaseType_t s_task_priority;
static BaseType_t s_task_core;
static uint32_t s_task_caps;
static esp_err_t s_get_active_result = ESP_OK;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length == OBJECT_SERVICE_QUEUE_LENGTH);
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
        object_service_command_t stop = {.type = OBJECT_SERVICE_COMMAND_STOP};
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

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core, uint32_t caps)
{
    (void)arg;
    assert(strcmp(name, "object_service") == 0);
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
    assert(ticks == pdMS_TO_TICKS(10));
    if (s_task_entry != NULL) {
        TaskFunction_t task = s_task_entry;
        s_task_entry = NULL;
        task(NULL);
    }
}

esp_err_t object_store_get_active(object_summary_t *object)
{
    if (s_get_active_result == ESP_OK) {
        memset(object, 0, sizeof(*object));
        strcpy(object->id, "OBJ_0001");
        object->active = true;
    }
    return s_get_active_result;
}

void object_service_internal_process(const object_service_command_t *command)
{
    object_service_state_t *state = object_service_internal_state();
    ++s_process_calls;
    s_process_saw_busy = state->busy;
    s_last_processed_type = command->type;
    memcpy(s_last_processed_id, command->object_id, sizeof(s_last_processed_id));
}

static void dummy_callback(const object_service_event_t *event, void *context) { (void)event; (void)context; }
static void dummy_observer(const object_service_event_t *event, void *context) { (void)event; (void)context; }

int main(void)
{
    object_service_summary_t active;
    object_service_state_t *state = object_service_internal_state();

    assert(!object_service_is_busy());
    assert(object_service_set_event_callback(dummy_callback, NULL) == ESP_ERR_INVALID_STATE);
    assert(object_service_set_event_observer(dummy_observer, NULL) == ESP_ERR_INVALID_STATE);
    assert(object_service_get_active(NULL) == ESP_ERR_INVALID_ARG);
    assert(object_service_get_active(&active) == ESP_ERR_INVALID_STATE);
    assert(object_service_create_async() == ESP_ERR_INVALID_STATE);

    s_fail_queue_create = true;
    assert(object_service_init() == ESP_ERR_NO_MEM);
    s_fail_queue_create = false;

    s_fail_task_create = true;
    assert(object_service_init() == ESP_ERR_NO_MEM);
    assert(s_queue_delete_calls == 1U);
    assert(state->command_queue == NULL);
    s_fail_task_create = false;

    assert(object_service_init() == ESP_OK);
    assert(object_service_init() == ESP_OK);
    assert(s_task_entry != NULL);
    assert(s_task_stack == APP_TASK_STACK_OBJECT_SERVICE);
    assert(s_task_priority == APP_TASK_PRIORITY_OBJECT_SERVICE);
    assert(s_task_core == APP_TASK_CORE_SERVICES);
    assert(s_task_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(object_service_set_event_callback(dummy_callback, &active) == ESP_OK);
    assert(object_service_set_event_observer(dummy_observer, state) == ESP_OK);

    assert(object_service_get_active(&active) == ESP_OK);
    assert(strcmp(active.id, "OBJ_0001") == 0 && active.active);
    s_get_active_result = ESP_FAIL;
    assert(object_service_get_active(&active) == ESP_FAIL);
    s_get_active_result = ESP_OK;

    assert(object_service_set_active_async(NULL) == ESP_ERR_INVALID_ARG);
    assert(object_service_set_active_async("") == ESP_ERR_INVALID_ARG);
    assert(object_service_set_active_async("1234567890123456") == ESP_ERR_INVALID_ARG);

    s_fail_queue_send = true;
    assert(object_service_list_async() == ESP_ERR_TIMEOUT);
    assert(!object_service_is_busy());
    s_fail_queue_send = false;

    assert(object_service_create_async() == ESP_OK);
    assert(object_service_is_busy());
    assert(object_service_list_async() == ESP_ERR_INVALID_STATE);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_process_calls == 1U);
    assert(s_process_saw_busy);
    assert(s_last_processed_type == OBJECT_SERVICE_COMMAND_CREATE);
    assert(!object_service_is_busy());

    object_service_shutdown();
    assert(!state->initialized);
    assert(state->command_queue == NULL);
    assert(state->event_callback == NULL && state->event_observer == NULL);
    assert(!state->busy);

    assert(object_service_init() == ESP_OK);
    assert(object_service_delete_async("OBJ_0002") == ESP_OK);
    s_inject_stop_when_empty = true;
    s_task_entry(NULL);
    s_task_entry = NULL;
    assert(s_last_processed_type == OBJECT_SERVICE_COMMAND_DELETE);
    assert(strcmp(s_last_processed_id, "OBJ_0002") == 0);
    object_service_shutdown();

    assert(object_service_init() == ESP_OK);
    object_service_shutdown();
    assert(!state->initialized);

    object_service_shutdown();
    return 0;
}
