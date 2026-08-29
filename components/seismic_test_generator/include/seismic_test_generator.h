#pragma once

#include "esp_err.h"


#define SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ  50


esp_err_t seismic_test_generator_start(void);


void seismic_test_generator_stop(void);
