#pragma once

#include <stdint.h>

#include "esp_err.h"

void main_barometer_request_polling(void);
esp_err_t main_barometer_request_height_reference_capture(uint16_t generation);
