#pragma once

#include <stdint.h>

typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;

void sensor_bus_log_diag_summary(uint32_t now_ms);
