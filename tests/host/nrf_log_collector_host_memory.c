#include <stdlib.h>

#include "esp_heap_caps.h"
#include "freertos/semphr.h"

struct test_log_mutex {
    int held;
};

static struct test_log_mutex s_mutex;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

void heap_caps_free(void *memory)
{
    free(memory);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    s_mutex.held = 0;
    return &s_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)ticks;
    if (semaphore == NULL || semaphore->held != 0) return pdFALSE;
    semaphore->held = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL || semaphore->held == 0) return pdFALSE;
    semaphore->held = 0;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}
