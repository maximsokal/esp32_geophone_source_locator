#pragma once

#include <stdint.h>

#include "host_threads.h"

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
typedef void *TaskHandle_t;
typedef unsigned EventBits_t;

typedef host_mutex_t portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define BIT0 (1U << 0)
#define taskYIELD() ((void)0)
#define portMUX_INITIALIZER_UNLOCKED HOST_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) test_port_exit_critical(mux)
#define portENTER_CRITICAL_ISR(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL_ISR(mux) test_port_exit_critical(mux)
#define portYIELD_FROM_ISR() test_port_yield_from_isr()

void test_port_enter_critical(portMUX_TYPE *mux);
void test_port_exit_critical(portMUX_TYPE *mux);
void test_port_yield_from_isr(void);
