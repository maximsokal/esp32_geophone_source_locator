#pragma once

#include <stddef.h>

typedef int BaseType_t;
typedef unsigned long TickType_t;
typedef unsigned UBaseType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY ((TickType_t)-1L)
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
#define portMUX_INITIALIZER_UNLOCKED 0

static inline unsigned *fake_critical_depth_storage(void)
{
    static unsigned depth;
    return &depth;
}

#define fake_critical_depth (*fake_critical_depth_storage())
#define taskENTER_CRITICAL(mux) \
    do { (void)(mux); ++fake_critical_depth; } while (0)
#define taskEXIT_CRITICAL(mux) \
    do { (void)(mux); --fake_critical_depth; } while (0)
