#ifndef ZEPHYR_DRIVERS_HWINFO_H
#define ZEPHYR_DRIVERS_HWINFO_H

#include <stdint.h>

int hwinfo_get_reset_cause(uint32_t *cause);

#endif
