#pragma once

#include <stddef.h>
#include <stdatomic.h>

typedef int BaseType_t;
typedef unsigned long TickType_t;
typedef unsigned UBaseType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef atomic_flag portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY ((TickType_t)-1L)
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
#define portMUX_INITIALIZER_UNLOCKED ATOMIC_FLAG_INIT

static _Thread_local unsigned fake_critical_depth;

static inline void test_task_enter_critical(portMUX_TYPE *mux)
{
    while (atomic_flag_test_and_set_explicit(mux, memory_order_acquire)) { }
    ++fake_critical_depth;
}

static inline void test_task_exit_critical(portMUX_TYPE *mux)
{
    --fake_critical_depth;
    atomic_flag_clear_explicit(mux, memory_order_release);
}

#define taskENTER_CRITICAL(mux) test_task_enter_critical(mux)
#define taskEXIT_CRITICAL(mux) test_task_exit_critical(mux)
