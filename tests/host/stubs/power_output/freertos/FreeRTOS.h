#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

/* 1 kHz tick: one tick is one millisecond of the test clock. */
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
