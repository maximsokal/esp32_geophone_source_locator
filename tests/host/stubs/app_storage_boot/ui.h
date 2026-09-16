#pragma once

#include "esp_err.h"

esp_err_t ui_boot_init(void);
void ui_boot_show_error(const char *message);
void ui_shutdown(void);
