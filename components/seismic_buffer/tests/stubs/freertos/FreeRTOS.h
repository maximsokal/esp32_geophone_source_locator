#pragma once

typedef struct { int unused; } portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }
#if defined(SEISMIC_BUFFER_TEST_HOOKS)
void seismic_buffer_test_enter_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) seismic_buffer_test_enter_critical(lock)
#else
#define portENTER_CRITICAL(lock) ((void)(lock))
#endif
#define portEXIT_CRITICAL(lock) ((void)(lock))
