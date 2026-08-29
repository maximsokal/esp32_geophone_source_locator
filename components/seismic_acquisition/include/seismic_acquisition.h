#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t seismic_acquisition_start(void);

void seismic_acquisition_stop(void);

bool seismic_acquisition_is_running(void);

const char *seismic_acquisition_source_name(void);
