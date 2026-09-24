#pragma once

#include <stdint.h>

/* Mirrors CONFIG_FREERTOS_HZ=100 of the firmware: one tick is 10 ms. */
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100U
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))

typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }

void test_port_enter_critical(portMUX_TYPE *mux);
void test_port_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) test_port_exit_critical(mux)
#define portENTER_CRITICAL_ISR(mux) test_port_enter_critical(mux)
#define portEXIT_CRITICAL_ISR(mux) test_port_exit_critical(mux)
