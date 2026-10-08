#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool seismic_acquisition_is_running(void);
esp_err_t seismic_acquisition_last_error(void);
const char *seismic_acquisition_source_name(void);
