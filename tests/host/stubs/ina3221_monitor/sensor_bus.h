#pragma once

#include <stdint.h>

/* Same constant the real header derives from Kconfig; the monitor only prints it. */
#define SENSOR_BUS_I2C_SPEED_HZ 100000U

typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;

void sensor_bus_log_diag_summary(uint32_t now_ms);
