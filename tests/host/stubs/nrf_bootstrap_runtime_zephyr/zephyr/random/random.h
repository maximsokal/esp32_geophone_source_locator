#ifndef ZEPHYR_RANDOM_RANDOM_H
#define ZEPHYR_RANDOM_RANDOM_H

#include <stddef.h>

int sys_csrand_get(void *destination, size_t length);

#endif
