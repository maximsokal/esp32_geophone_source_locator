#pragma once
/* One FreeRTOS surface for the end-to-end barometer / INA3221 / sensor_bus test:
 * the union of what sensor_bus.c, ina3221_monitor.c and main_barometer.c need. */
#include <stdatomic.h>
#include <stdint.h>

typedef struct test_semaphore *SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef struct { atomic_flag flag; } portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
/* The firmware tick: 10 ms. sensor_bus asserts its timeouts against this. */
#define configTICK_RATE_HZ 100U
#define CONFIG_I2C_MS_TO_WAIT 200U
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(((uint64_t)(milliseconds) * configTICK_RATE_HZ) / 1000U))
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
