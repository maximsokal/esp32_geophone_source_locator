#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "freertos/queue.h"
#include "freertos/idf_additions.h"

typedef struct {
    unsigned length;
    unsigned item_size;
    unsigned head;
    unsigned count;
    unsigned char *items;
} host_queue_t;

static void (*s_created_task)(void *);
static void *s_created_argument;
static unsigned s_external_delete_calls;
static unsigned s_queue_delete_calls;
#ifdef _WIN32
static HANDLE s_task_thread;
#else
static pthread_t s_task_thread;
static int s_task_thread_started;
#endif

unsigned workflow_host_rtos_external_delete_count(void) { return s_external_delete_calls; }
unsigned workflow_host_rtos_queue_delete_count(void) { return s_queue_delete_calls; }

static void workflow_host_rtos_run_task(void)
{
    assert(s_created_task != NULL);
    s_created_task(s_created_argument);
}

#ifdef _WIN32
static DWORD WINAPI workflow_host_rtos_task_entry(LPVOID argument)
{
    (void)argument;
    workflow_host_rtos_run_task();
    return 0U;
}
#else
static void *workflow_host_rtos_task_entry(void *argument)
{
    (void)argument;
    workflow_host_rtos_run_task();
    return NULL;
}
#endif

void workflow_host_rtos_start_created_task(void)
{
    assert(s_created_task != NULL);
#ifdef _WIN32
    assert(s_task_thread == NULL);
    s_task_thread = CreateThread(NULL, 0U, workflow_host_rtos_task_entry, NULL, 0U, NULL);
    assert(s_task_thread != NULL);
#else
    assert(!s_task_thread_started);
    assert(pthread_create(&s_task_thread, NULL, workflow_host_rtos_task_entry, NULL) == 0);
    s_task_thread_started = 1;
#endif
}

void workflow_host_rtos_run_created_task_sync(void)
{
    workflow_host_rtos_run_task();
}

int workflow_host_rtos_task_thread_running(void)
{
#ifdef _WIN32
    return s_task_thread != NULL &&
        WaitForSingleObject(s_task_thread, 0U) == WAIT_TIMEOUT;
#else
    return s_task_thread_started;
#endif
}

void workflow_host_rtos_join_created_task(void)
{
#ifdef _WIN32
    if (s_task_thread != NULL) {
        assert(WaitForSingleObject(s_task_thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(s_task_thread);
        s_task_thread = NULL;
    }
#else
    if (s_task_thread_started) {
        assert(pthread_join(s_task_thread, NULL) == 0);
        s_task_thread_started = 0;
    }
#endif
}

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
    ++s_queue_delete_calls;
    free(queue->items);
    free(queue);
}

TickType_t xTaskGetTickCount(void) { return 0U; }
void vTaskDelayUntil(TickType_t *last_wake, TickType_t period)
{
    if (last_wake != NULL) *last_wake += period;
}
void vTaskDelete(TaskHandle_t task)
{
    if (task != NULL) {
        ++s_external_delete_calls;
        return;
    }
#ifdef _WIN32
    if (s_task_thread != NULL && GetCurrentThreadId() == GetThreadId(s_task_thread)) {
        ExitThread(0U);
    }
#else
    if (s_task_thread_started && pthread_equal(pthread_self(), s_task_thread)) {
        pthread_exit(NULL);
    }
#endif
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 3072U; }
BaseType_t xTaskCreatePinnedToCoreWithCaps(
    void (*task)(void *), const char *name, uint32_t stack_bytes, void *argument,
    unsigned priority, TaskHandle_t *task_out, int core_id, unsigned caps)
{
    (void)name; (void)stack_bytes;
    (void)priority; (void)core_id; (void)caps;
    s_created_task = task;
    s_created_argument = argument;
    if (task_out != NULL) *task_out = (TaskHandle_t)(uintptr_t)1U;
    return pdPASS;
}
