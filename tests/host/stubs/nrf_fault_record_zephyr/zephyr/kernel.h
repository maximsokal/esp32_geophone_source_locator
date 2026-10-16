#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H

#include <stdbool.h>

struct k_thread { unsigned unused; };

struct k_thread *k_current_get(void);
bool k_is_in_isr(void);
const char *k_thread_name_get(struct k_thread *thread);

#endif
