#pragma once

#include <stdint.h>

#include "esp_err.h"

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320

esp_err_t display_init(void);

esp_err_t display_fill(uint16_t color);

void display_deinit(void);