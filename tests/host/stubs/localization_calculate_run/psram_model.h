#pragma once

#include <stddef.h>

void localization_test_psram_model_set_reserved(size_t reserved_bytes);
void localization_test_psram_model_reset(void);
size_t localization_test_psram_model_free_bytes(void);
size_t localization_test_psram_model_allocated_bytes(void);
