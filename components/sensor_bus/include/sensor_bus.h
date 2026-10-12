#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"
#include "i2c_line_recovery.h"
#include "sensor_bus_errors.h"


/*
 * Shared sensor I2C0 contract.
 *
 * The pins and electrical pull-up policy remain board-owned in board_config.h.
 * This component is the only application owner that creates or deletes I2C0.
 * Consumers borrow returned device handles and must not delete them.
 */
#define SENSOR_BUS_I2C_SPEED_HZ  100000U
#define SENSOR_BUS_TRANSACTION_TIMEOUT_MS 20U
#define SENSOR_BUS_MUTEX_WAIT_MS 200U
#define SENSOR_BUS_OWNER_MAX_HOLD_MS 150U

#define SENSOR_BUS_TASK_NAME_SIZE 16U


typedef struct
{
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t ina3221;

} sensor_bus_handles_t;

typedef struct {
    uint32_t busy_count;
    uint32_t recover_attempts;
    uint64_t last_waited_for_mutex_us;
    uint64_t last_hold_us;
    char holder_task[SENSOR_BUS_TASK_NAME_SIZE];
} sensor_bus_owner_stats_t;


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

/* Priority-inheriting, bounded-wait owner lock. The owner may call other
 * sensor_bus APIs recursively while holding it. Use it across a TCA
 * select -> downstream transfer(s) -> close sequence. */
esp_err_t sensor_bus_owner_acquire(void);
void sensor_bus_owner_release(void);
bool sensor_bus_owner_hold_budget_exceeded(void);
esp_err_t sensor_bus_get_owner_stats(sensor_bus_owner_stats_t *stats);

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

/* Opaque sensor_bus-owned handles preserve the existing consumer type, while
 * transfers use the IDF master API with millisecond timeouts. */
esp_err_t sensor_bus_device_create(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed_hz,
    i2c_bus_device_handle_t *device);
esp_err_t sensor_bus_device_delete(i2c_bus_device_handle_t *device);
esp_err_t sensor_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data);
esp_err_t sensor_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data);
esp_err_t sensor_bus_read_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data);
esp_err_t sensor_bus_write_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    const uint8_t *data);

/* Short (20 ms) address-only probe on the shared bus. A NACK is
 * ESP_ERR_NOT_FOUND; a busy/stuck bus is reported as ESP_ERR_TIMEOUT. */
esp_err_t sensor_bus_probe_address(uint8_t address);

/* Context variants add source/channel to timeout diagnostics; -1 means direct.
 * The source pointer must remain stable for rate limiting. */
esp_err_t sensor_bus_probe_address_with_context(
    uint8_t address,
    const char *source,
    int channel
);

/*
 * MAX-248: map a failed transfer to "nobody answered at this address".
 *
 * The i2c_bus v2 backend (ESP-IDF >= 5.3) returns ESP_ERR_INVALID_STATE
 * for a NACK -- the same code as a hardware timeout -- while the legacy
 * driver returned ESP_FAIL. Given the failed transfer's error, this
 * returns ESP_ERR_NOT_FOUND when the address NACKs (legacy ESP_FAIL, or an
 * address-only probe that finds nobody), and otherwise the error that
 * describes the fault: the original error when the address ACKs (target
 * present, transfer failed), or the probe's error (e.g. ESP_ERR_TIMEOUT
 * for a busy/stuck bus). ESP_OK and non-bus errors pass through untouched.
 */
esp_err_t sensor_bus_classify_transfer_error(
    uint8_t address,
    esp_err_t transfer_err
);

esp_err_t sensor_bus_classify_transfer_error_with_context(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel
);

/* Record a raw result without probing; raw INVALID_STATE remains unclassified. */
void sensor_bus_record_transfer_result_with_context(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel
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

/* Emit at most one bounded summary block per 10 s, only after recent failures. */
void sensor_bus_log_diag_summary(uint32_t now_ms);

/*
 * Release the shared bus after every consumer has stopped. Owned device
 * handles are deleted before the I2C0 bus itself.
 */
esp_err_t sensor_bus_deinit(void);
