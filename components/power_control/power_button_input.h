#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t power_button_input_init(void);


bool power_button_input_is_high(void);
