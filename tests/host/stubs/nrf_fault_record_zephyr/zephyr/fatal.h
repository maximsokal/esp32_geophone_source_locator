#ifndef ZEPHYR_FATAL_H
#define ZEPHYR_FATAL_H

#include <stdint.h>

#define K_ERR_CPU_EXCEPTION 1U
#define K_ERR_STACK_CHK_FAIL 2U

struct arch_esf {
    struct {
        uint32_t pc;
        uint32_t lr;
    } basic;
};

#endif
