#pragma once

typedef struct {
    unsigned unused;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0U }
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
