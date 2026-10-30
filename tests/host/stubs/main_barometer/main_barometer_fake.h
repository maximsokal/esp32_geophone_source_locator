#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_status.h"
#include "main_barometer.h"

/* Scripted stand-in for main_barometer.c and the device_status_set_main() sink. */
typedef struct {
    unsigned init_calls;
    unsigned poll_calls;
    unsigned close_calls;
    uint64_t last_poll_ms;
    esp_err_t poll_result;
    main_barometer_reading_t reading;
    bool polling_requested;
    /* Non-zero: poll() opens the "bus" and schedules the next poll this far ahead. */
    uint32_t next_poll_delay_ms;
    void (*on_poll)(void);
    unsigned set_main_calls;
    esp_err_t set_main_result;
    device_main_status_t last_main_status;
} main_barometer_fake_t;

extern main_barometer_fake_t g_main_barometer_fake;

void main_barometer_fake_reset(void);
