#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "i2c_line_recovery.h"

/*
 * MAX-243: internal platform seam of sensor_bus for the SDA/SCL lines
 * (sensor_bus_lines.c on the target, a line-model fake in host tests).
 * Not for other components.
 */

/* Take SDA/SCL as open-drain GPIOs with pull-ups, both released, and fill
 * `io` for a bit-bang bus clear. Only while no I2C driver owns the pins. */
esp_err_t sensor_bus_lines_bitbang_io(i2c_line_io_t *io);

/* Sample the line levels while the I2C driver owns the pins. */
void sensor_bus_lines_read(bool *sda_high, bool *scl_high);

/* Controller-side bus clear (clocks + STOP + FSM reset) of a live bus. */
esp_err_t sensor_bus_lines_controller_reset(i2c_bus_handle_t bus);

void sensor_bus_lines_delay_us(uint32_t us);
int64_t sensor_bus_lines_now_us(void);
