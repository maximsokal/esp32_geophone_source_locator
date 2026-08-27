#pragma once

#include "esp_err.h"


esp_err_t seismic_acquisition_start(void);

void seismic_acquisition_stop(void);

const char *seismic_acquisition_source_name(void);
