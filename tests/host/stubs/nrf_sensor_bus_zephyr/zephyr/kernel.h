#pragma once

#include <stdint.h>

int32_t k_msleep(int32_t milliseconds);
int64_t k_uptime_get(void);

typedef unsigned int k_spinlock_key_t;
struct k_spinlock { unsigned unused; };
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) { (void)lock; return 0U; }
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) { (void)lock; (void)key; }
