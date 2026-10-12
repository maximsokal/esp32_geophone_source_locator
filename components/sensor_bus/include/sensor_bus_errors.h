#pragma once

#include "esp_err.h"

/* A bounded owner-lock miss is not evidence of an I2C transaction failure. */
#define SENSOR_BUS_ERR_MUTEX_BUSY ((esp_err_t)0x7F01)
#define SENSOR_BUS_ERR_OWNER_HOLD_BUDGET ((esp_err_t)0x7F02)
