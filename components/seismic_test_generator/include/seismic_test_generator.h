#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/* The rate of the next run: a run-time choice like the AD7606's (only while the generator is stopped). */
uint32_t seismic_test_generator_get_rate(void);
bool seismic_test_generator_rate_supported(uint32_t sample_rate_hz);
esp_err_t seismic_test_generator_set_rate(uint32_t sample_rate_hz);


void seismic_test_generator_stop(void);
