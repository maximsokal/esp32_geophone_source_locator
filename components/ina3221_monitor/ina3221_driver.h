#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "ina3221_limits.h"
#include "ina3221_policy.h"
#include "sensor_bus.h"


typedef struct
{
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t device;
    bool ready;

} ina3221_driver_t;


esp_err_t ina3221_driver_open(
    ina3221_driver_t *driver
);

void ina3221_driver_close(
    ina3221_driver_t *driver
);

/*
 * MAX-245: write the Critical/Warning alert limits of all three channels
 * (disabled channels get INA3221_LIMITS_REG_DISABLED) and read every one
 * back. Invalid limits are rejected before any write.
 */
esp_err_t ina3221_driver_program_limits(
    ina3221_driver_t *driver,
    const ina3221_limits_t *limits
);

/* Raw Mask/Enable register (0x0F): CF1..CF3 = bits 9..7, WF1..WF3 =
 * bits 5..3. CRI/WAR are not wired, so this is the only way to see them. */
#define INA3221_DRIVER_MASK_CF1 (1U << 9)
#define INA3221_DRIVER_MASK_WF1 (1U << 5)

esp_err_t ina3221_driver_read_alert_flags(
    ina3221_driver_t *driver,
    uint16_t *mask_enable
);

esp_err_t ina3221_driver_read_channels(
    ina3221_driver_t *driver,
    ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT]
);
