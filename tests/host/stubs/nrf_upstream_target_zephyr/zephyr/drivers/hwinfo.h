#ifndef ZEPHYR_DRIVERS_HWINFO_H
#define ZEPHYR_DRIVERS_HWINFO_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
typedef ptrdiff_t ssize_t;
#else
#include <sys/types.h>
#endif

ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length);

#endif
