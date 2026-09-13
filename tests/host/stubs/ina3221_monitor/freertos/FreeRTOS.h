#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct {
    unsigned depth;
} portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED { 0U }

void test_port_enter_critical(portMUX_TYPE *mux);
void test_port_exit_critical(portMUX_TYPE *mux);

#define portENTER_CRITICAL(mux) test_port_enter_critical((mux))
#define portEXIT_CRITICAL(mux) test_port_exit_critical((mux))
