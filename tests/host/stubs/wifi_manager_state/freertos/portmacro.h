#pragma once

#include <stdint.h>

typedef struct
{
    uint32_t depth;

} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0U }

void test_port_enter_critical(portMUX_TYPE *mux);
void test_port_exit_critical(portMUX_TYPE *mux);

#define portENTER_CRITICAL(mux) test_port_enter_critical((mux))
#define portEXIT_CRITICAL(mux) test_port_exit_critical((mux))
