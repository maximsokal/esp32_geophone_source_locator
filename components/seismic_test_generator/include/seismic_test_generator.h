#pragma once

#include "esp_err.h"


#define SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ  50

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
#include "app_test_recording_scenario.h"
/* The rate the synthetic source really runs at: the recording scenario records at the board's 500 Hz. */
#define SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ TEST_REC_SAMPLE_RATE_HZ
#else
#define SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ SEISMIC_TEST_GENERATOR_SAMPLE_RATE_HZ
#endif


esp_err_t seismic_test_generator_start(void);


void seismic_test_generator_stop(void);
