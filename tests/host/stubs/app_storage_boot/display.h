#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t display_init(void);
esp_err_t display_set_enabled(bool enabled);
void display_deinit(void);
