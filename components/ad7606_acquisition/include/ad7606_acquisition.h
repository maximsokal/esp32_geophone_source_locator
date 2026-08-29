#pragma once

#include "esp_err.h"


esp_err_t ad7606_acquisition_start(void);

void ad7606_acquisition_stop(void);