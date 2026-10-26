#pragma once

#include <stdbool.h>

#include "nrf_sensor_cache.h"

typedef struct {
    bool temperature_valid;
    float temperature_c;
    bool humidity_valid;
    float humidity_percent;
    bool gyro_valid;
    float gyro_rate_dps;
} app_pod_status_sensor_values_t;

/*
 * Pure mapping from one nRF sensor-cache entry to the measured values the
 * pod status bridge publishes: BME280 temperature/humidity (ENV domain) and the
 * BMI160 angular-rate magnitude (IMU domain). Each is valid only for a real,
 * current response whose own domain is flagged valid and not faulted; nothing
 * is derived or invented. When the IMU is calibrated the stored gyro bias is
 * subtracted so a still pod reads about zero.
 */
app_pod_status_sensor_values_t app_pod_status_bridge_compute_sensor_values(
    const nrf_remote_sensor_cache_t *cache);
