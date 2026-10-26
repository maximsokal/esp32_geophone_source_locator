#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "i2c_line_recovery.h"

#define SENSOR_BUS_LINES_PROBE_TIMEOUT_MS 20

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

/* Address-only probe (START + address + STOP): ESP_OK when a target ACKs,
 * ESP_ERR_NOT_FOUND on NACK, ESP_ERR_TIMEOUT when the bus is busy/stuck. */
esp_err_t sensor_bus_lines_probe(i2c_bus_handle_t bus, uint8_t address);

/*
 * The IDF master driver runs transfers of 31 bytes or more in FIFO-sized
 * segments, and in blocking mode the calling task has to be scheduled at every
 * segment boundary while the controller holds SCL low. Raise the caller to
 * `priority` for one such transfer; the previous priority comes back from the
 * first call and goes into the second. A priority that is not above the current
 * one changes nothing.
 */
uint32_t sensor_bus_lines_boost_task_priority(uint32_t priority);
void sensor_bus_lines_restore_task_priority(uint32_t previous_priority);

void sensor_bus_lines_delay_us(uint32_t us);
int64_t sensor_bus_lines_now_us(void);
