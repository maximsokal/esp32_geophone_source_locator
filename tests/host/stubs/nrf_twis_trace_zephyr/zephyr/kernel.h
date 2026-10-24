#pragma once

#include <stdint.h>

uint32_t k_cycle_get_32(void);
static inline uint32_t k_cyc_to_us_floor32(uint32_t cycles) { return cycles; }
