#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/queue.h"
#include "freertos/idf_additions.h"

typedef struct {
    unsigned length;
    unsigned item_size;
    unsigned head;
    unsigned count;
    unsigned char *items;
} host_queue_t;

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    host_queue_t *queue = calloc(1U, sizeof(*queue));
    if (queue == NULL) return NULL;
    queue->items = calloc(length, item_size);
    if (queue->items == NULL) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout)
{
    assert(timeout == 0U);
    host_queue_t *queue = handle;
    if (queue == NULL || item == NULL || queue->count == queue->length) return pdFALSE;
    const unsigned tail = (queue->head + queue->count) % queue->length;
    memcpy(queue->items + (size_t)tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout)
{
    assert(timeout == 0U);
    host_queue_t *queue = handle;
    if (queue == NULL || item == NULL || queue->count == 0U) return pdFALSE;
    memcpy(item, queue->items + (size_t)queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1U) % queue->length;
    --queue->count;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t handle)
{
    host_queue_t *queue = handle;
    if (queue == NULL) return;
    free(queue->items);
    free(queue);
}

TickType_t xTaskGetTickCount(void) { return 0U; }
void vTaskDelayUntil(TickType_t *last_wake, TickType_t period)
{
    if (last_wake != NULL) *last_wake += period;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 3072U; }
BaseType_t xTaskCreatePinnedToCoreWithCaps(
    void (*task)(void *), const char *name, uint32_t stack_bytes, void *argument,
    unsigned priority, TaskHandle_t *task_out, int core_id, unsigned caps)
{
    (void)task; (void)name; (void)stack_bytes; (void)argument;
    (void)priority; (void)core_id; (void)caps;
    if (task_out != NULL) *task_out = (TaskHandle_t)(uintptr_t)1U;
    return pdPASS;
}
