#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

typedef struct {
    int unused;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
