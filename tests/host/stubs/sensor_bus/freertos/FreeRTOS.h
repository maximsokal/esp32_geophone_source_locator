#pragma once
#include <stdatomic.h>
#include <stdint.h>
typedef struct test_semaphore *SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef struct { atomic_flag flag; } portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define portMUX_INITIALIZER_UNLOCKED { ATOMIC_FLAG_INIT }
static inline void test_port_enter_critical(portMUX_TYPE *mux)
{
    while (atomic_flag_test_and_set_explicit(&mux->flag, memory_order_acquire)) { }
}
static inline void test_port_exit_critical(portMUX_TYPE *mux)
{
    atomic_flag_clear_explicit(&mux->flag, memory_order_release);
}
#define portENTER_CRITICAL(mux) test_port_enter_critical((mux))
#define portEXIT_CRITICAL(mux) test_port_exit_critical((mux))
