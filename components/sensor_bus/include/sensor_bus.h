#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"


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
 * Release the shared bus after every consumer has stopped. Owned device
 * handles are deleted before the I2C0 bus itself.
 */
esp_err_t sensor_bus_deinit(void);
