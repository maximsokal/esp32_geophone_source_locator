#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "i2c_line_recovery.h"


/*
 * Shared sensor I2C0 contract.
 *
 * The pins and electrical pull-up policy remain board-owned in board_config.h.
 * This component is the only application owner that creates or deletes I2C0.
 * Consumers borrow returned device handles and must not delete them.
 */
#define SENSOR_BUS_I2C_SPEED_HZ  100000U


typedef struct
{
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t ina3221;

} sensor_bus_handles_t;


/*
 * Create the state mutex that serializes every other function in this file.
 * Must be called exactly once, synchronously, before any task that might
 * call sensor_bus_init()/get_handles()/get_tca9548a()/deinit() is started
 * (main/app_startup.c calls this from the single-threaded boot path). This
 * avoids a lazy-init race on the mutex itself: nothing below may create the
 * lock on first use, because "first use" can happen concurrently from the
 * INA3221 monitor task and the nRF topology service task.
 */
esp_err_t sensor_bus_lock_init(void);

/* Create the shared I2C0 bus and the currently contracted INA3221 device. */
esp_err_t sensor_bus_init(void);

/*
 * Return borrowed handles. This lazily initializes the shared bus so existing
 * startup ordering does not need a second application startup hook.
 */
esp_err_t sensor_bus_get_handles(
    sensor_bus_handles_t *handles
);

/*
 * Lazily discover the optional TCA9548A only in 0x70..0x77 and return its
 * borrowed sensor_bus-owned handle. Exactly one responding candidate is
 * required. The caller must not delete the returned handle.
 */
esp_err_t sensor_bus_get_tca9548a(
    i2c_bus_device_handle_t *device,
    uint8_t *address
);

/*
 * MAX-243 stuck-bus handling (NXP UM10204 bus clear).
 *
 * Before I2C0 is created, sensor_bus_init() bit-bangs a bus clear on the
 * still-free pins. After a transfer timeout a consumer calls
 * sensor_bus_recover(): SDA held low -> controller bus clear (up to 9 SCL
 * clocks + STOP); SCL held low -> reported as a target fault WITHOUT
 * clocking (only a reset/power cycle of that target clears it). Attempts
 * are rate-limited by i2c_line_recovery_backoff_allows(); a refused
 * attempt returns ESP_ERR_INVALID_STATE without touching the bus.
 *
 * Returns ESP_OK when both lines are high afterwards (IDLE or RECOVERED),
 * ESP_ERR_TIMEOUT when a line stays stuck. `result` may be NULL.
 */
typedef struct
{
    uint32_t attempts;
    uint32_t recoveries;
    uint32_t failures;
    uint32_t consecutive_failures;
    i2c_line_result_t last_result;
    int64_t last_attempt_us;

} sensor_bus_recovery_status_t;

esp_err_t sensor_bus_recover(
    i2c_line_result_t *result
);

esp_err_t sensor_bus_get_recovery_status(
    sensor_bus_recovery_status_t *status
);

/*
 * Release the shared bus after every consumer has stopped. Owned device
 * handles are deleted before the I2C0 bus itself.
 */
esp_err_t sensor_bus_deinit(void);
