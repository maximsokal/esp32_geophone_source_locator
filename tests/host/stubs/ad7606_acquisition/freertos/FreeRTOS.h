#pragma once

#include <stdint.h>

#include "host_threads.h"

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFU
#define configTICK_RATE_HZ 100U
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
void test_port_yield_from_isr(void);
#define portYIELD_FROM_ISR() test_port_yield_from_isr()

typedef host_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED HOST_MUTEX_INITIALIZER

void test_port_enter_critical(portMUX_TYPE *mux);
void test_port_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) test_port_exit_critical(mux)
#define portENTER_CRITICAL_ISR(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL_ISR(mux) test_port_exit_critical(mux)
