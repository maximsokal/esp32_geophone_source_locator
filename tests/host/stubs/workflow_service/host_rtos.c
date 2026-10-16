#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "host_rtos.h"

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
static TickType_t s_tick_count;
static TickType_t s_last_delay_wait;
static TickType_t s_last_delay_period;
static bool s_delay_barrier_enabled;
static bool s_delay_barrier_entered;
static bool s_delay_barrier_released;
#ifdef _WIN32
static HANDLE s_task_thread;
static SRWLOCK s_delay_barrier_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_delay_barrier_changed = CONDITION_VARIABLE_INIT;
#else
static pthread_t s_task_thread;
static int s_task_thread_started;
static pthread_mutex_t s_delay_barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_delay_barrier_changed = PTHREAD_COND_INITIALIZER;
#endif

unsigned workflow_host_rtos_external_delete_count(void) { return s_external_delete_calls; }
unsigned workflow_host_rtos_queue_delete_count(void) { return s_queue_delete_calls; }

void workflow_host_rtos_reset_ticks(void)
{
    s_tick_count = 0U;
    s_last_delay_wait = 0U;
    s_last_delay_period = 0U;
    s_delay_barrier_enabled = false;
    s_delay_barrier_entered = false;
    s_delay_barrier_released = false;
}

void workflow_host_rtos_advance_ticks(TickType_t ticks)
{
    s_tick_count += ticks;
}

TickType_t workflow_host_rtos_last_delay_wait(void) { return s_last_delay_wait; }
TickType_t workflow_host_rtos_last_delay_period(void) { return s_last_delay_period; }

void workflow_host_rtos_delay_barrier_enable(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_delay_barrier_lock) == 0);
#endif
    s_delay_barrier_enabled = true;
    s_delay_barrier_entered = false;
    s_delay_barrier_released = false;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_unlock(&s_delay_barrier_lock) == 0);
#endif
}

void workflow_host_rtos_delay_barrier_wait(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_delay_barrier_lock);
    while (!s_delay_barrier_entered) {
        assert(SleepConditionVariableSRW(
            &s_delay_barrier_changed, &s_delay_barrier_lock, INFINITE, 0U));
    }
    ReleaseSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_delay_barrier_lock) == 0);
    while (!s_delay_barrier_entered) {
        assert(pthread_cond_wait(&s_delay_barrier_changed, &s_delay_barrier_lock) == 0);
    }
    assert(pthread_mutex_unlock(&s_delay_barrier_lock) == 0);
#endif
}

void workflow_host_rtos_delay_barrier_release(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_delay_barrier_lock);
    s_delay_barrier_released = true;
    WakeAllConditionVariable(&s_delay_barrier_changed);
    ReleaseSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_delay_barrier_lock) == 0);
    s_delay_barrier_released = true;
    assert(pthread_cond_broadcast(&s_delay_barrier_changed) == 0);
    assert(pthread_mutex_unlock(&s_delay_barrier_lock) == 0);
#endif
}

void workflow_host_rtos_delay_barrier_disable(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_delay_barrier_lock) == 0);
#endif
    s_delay_barrier_enabled = false;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_unlock(&s_delay_barrier_lock) == 0);
#endif
}

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

TickType_t xTaskGetTickCount(void) { return s_tick_count; }
void vTaskDelayUntil(TickType_t *last_wake, TickType_t period)
{
    if (last_wake == NULL) return;
    *last_wake += period;
    s_last_delay_period = period;
    const TickType_t until_deadline = *last_wake - s_tick_count;
    s_last_delay_wait = until_deadline < UINT32_C(0x80000000) ? until_deadline : 0U;
    s_tick_count += s_last_delay_wait;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_delay_barrier_lock);
    if (s_delay_barrier_enabled) {
        s_delay_barrier_entered = true;
        WakeAllConditionVariable(&s_delay_barrier_changed);
        while (!s_delay_barrier_released) {
            assert(SleepConditionVariableSRW(
                &s_delay_barrier_changed, &s_delay_barrier_lock, INFINITE, 0U));
        }
    }
    ReleaseSRWLockExclusive(&s_delay_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_delay_barrier_lock) == 0);
    if (s_delay_barrier_enabled) {
        s_delay_barrier_entered = true;
        assert(pthread_cond_broadcast(&s_delay_barrier_changed) == 0);
        while (!s_delay_barrier_released) {
            assert(pthread_cond_wait(&s_delay_barrier_changed, &s_delay_barrier_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&s_delay_barrier_lock) == 0);
#endif
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
