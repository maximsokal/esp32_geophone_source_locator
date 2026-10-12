#include "sensor_bus.h"
#include "sensor_bus_diag.h"
#include "sensor_bus_lines.h"
#include "sensor_bus_probe_diag.h"
#include "board_config.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SENSOR_BUS_DEVICE_LIMIT 16U
#define SENSOR_BUS_WRITE_STACK_BYTES 32U
#define SENSOR_BUS_MUX_CLOSE_RESERVE_MS (2U * SENSOR_BUS_TRANSACTION_TIMEOUT_MS)

#define SENSOR_BUS_I2C_PORT I2C_NUM_0
#define SENSOR_BUS_TCA9548A_ADDR_MIN 0x70U
#define SENSOR_BUS_TCA9548A_ADDR_MAX 0x77U
#define SENSOR_BUS_BMP280_ADDR_MIN 0x76U
#define SENSOR_BUS_BMP280_ADDR_MAX 0x77U
/* espressif__i2c_bus v2 currently builds its i2c_master config with 7. */
#define SENSOR_BUS_I2C_GLITCH_IGNORE_COUNT 7U

static const char *TAG = "sensor_bus";
static const char s_tca_diag_source[] = "tca9548a";
static const char s_nrf_diag_source[] = "nrf_topology";

static i2c_bus_handle_t s_bus = NULL;
static i2c_bus_device_handle_t s_ina3221 = NULL;
static i2c_bus_device_handle_t s_tca9548a = NULL;
static uint8_t s_tca9548a_address = NULL_I2C_DEV_ADDR;
static sensor_bus_diag_state_t s_diag_state;
static sensor_bus_diag_stats_t s_diag_stats;
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_owner_stats_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    i2c_bus_device_handle_t bus_device;
    i2c_master_dev_handle_t master_device;
    uint8_t address;
    bool used;
} sensor_bus_device_entry_t;

static sensor_bus_device_entry_t s_devices[SENSOR_BUS_DEVICE_LIMIT];
static TaskHandle_t s_owner_task;
static uint32_t s_owner_depth;
static int64_t s_owner_started_us;
static uint32_t s_owner_session_depth;
static int64_t s_owner_session_started_us;
static bool s_owner_hold_budget_exceeded;
static bool s_owner_close_in_progress;
static sensor_bus_owner_stats_t s_owner_stats;

/*
 * s_bus/s_ina3221/s_tca9548a/s_tca9548a_address are shared mutable state
 * read and written from at least two independent FreeRTOS tasks (the
 * INA3221 monitor worker and the nRF topology service worker), both of
 * which call sensor_bus_init() on their own schedule. Every public function
 * below holds this mutex across its full body, including the blocking
 * vendor bus/device creation calls and I2C transactions, so it must be a
 * priority-inheriting task mutex, not a portMUX spinlock: ESP-IDF requires
 * blocking work to stay outside a critical section.
 */
static SemaphoreHandle_t s_lock = NULL;

/* MAX-243: guarded by s_lock. */
static sensor_bus_recovery_status_t s_recovery;

/* A live bus is judged stuck only if a line reads low on every sample over
 * ~1 ms: at 100 kHz a transfer of another task toggles SCL every 5 us, so a
 * normal transaction or clock stretch cannot look stuck. */
#define SENSOR_BUS_STUCK_SAMPLES 10U
#define SENSOR_BUS_STUCK_SAMPLE_US 100U

#ifndef CONFIG_APP_I2C_PROBE_DIAGNOSTICS
#define CONFIG_APP_I2C_PROBE_DIAGNOSTICS 0
#endif

#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
static esp_err_t sensor_bus_probe_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    const uint8_t address = *(const uint8_t *)context;
    (void)tx;
    (void)tx_len;
    (void)rx;
    (void)rx_len;
    return sensor_bus_lines_probe(s_bus, address);
}

static esp_err_t sensor_bus_read_byte_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)tx;
    (void)tx_len;
    if (rx == NULL || rx_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return sensor_bus_read_byte(
        (i2c_bus_device_handle_t)context, NULL_I2C_MEM_ADDR, rx);
}

static esp_err_t sensor_bus_write_byte_transfer(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len)
{
    (void)rx;
    (void)rx_len;
    if (tx == NULL || tx_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return sensor_bus_write_byte(
        (i2c_bus_device_handle_t)context, NULL_I2C_MEM_ADDR, tx[0]);
}
#endif

static esp_err_t sensor_bus_probe_address_raw(uint8_t address, int channel)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    return sensor_bus_probe_diag_transaction(
        SENSOR_BUS_PROBE_PHASE_PROBE, address, channel,
        NULL, 0U, NULL, 0U, false, 0U,
        sensor_bus_probe_transfer, &address);
#else
    (void)channel;
    return sensor_bus_lines_probe(s_bus, address);
#endif
}

static esp_err_t sensor_bus_read_byte_diagnostic(
    i2c_bus_device_handle_t device,
    uint8_t address,
    int channel,
    bool expected_valid,
    uint8_t expected,
    uint8_t *value)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    return sensor_bus_probe_diag_transaction(
        SENSOR_BUS_PROBE_PHASE_TCA_VERIFY, address, channel,
        NULL, 0U, value, 1U, expected_valid, expected,
        sensor_bus_read_byte_transfer, device);
#else
    (void)address;
    (void)channel;
    (void)expected_valid;
    (void)expected;
    return sensor_bus_read_byte(device, NULL_I2C_MEM_ADDR, value);
#endif
}

static esp_err_t sensor_bus_write_byte_diagnostic(
    i2c_bus_device_handle_t device,
    uint8_t address,
    int channel,
    sensor_bus_probe_phase_t phase,
    uint8_t value)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    return sensor_bus_probe_diag_transaction(
        phase, address, channel, &value, 1U, NULL, 0U, true, value,
        sensor_bus_write_byte_transfer, device);
#else
    (void)address;
    (void)channel;
    (void)phase;
    return sensor_bus_write_byte(device, NULL_I2C_MEM_ADDR, value);
#endif
}

static esp_err_t sensor_bus_device_delete_locked(i2c_bus_device_handle_t *device)
{
    if (device == NULL || *device == NULL) {
        return ESP_OK;
    }

    sensor_bus_device_entry_t *entry = NULL;
    for (size_t i = 0U; i < SENSOR_BUS_DEVICE_LIMIT; ++i) {
        if (s_devices[i].used && s_devices[i].bus_device == *device) {
            entry = &s_devices[i];
            break;
        }
    }
    if (entry != NULL && entry->master_device != NULL) {
        const esp_err_t remove_err = i2c_master_bus_rm_device(entry->master_device);
        if (remove_err != ESP_OK) {
            return remove_err;
        }
        entry->master_device = NULL;
    }

    if (entry == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(entry, 0, sizeof(*entry));
    *device = NULL;
    return ESP_OK;
}

static esp_err_t sensor_bus_device_create_locked(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed_hz,
    i2c_bus_device_handle_t *device)
{
    if (bus == NULL || bus != s_bus || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = NULL;

    sensor_bus_device_entry_t *entry = NULL;
    for (size_t i = 0U; i < SENSOR_BUS_DEVICE_LIMIT; ++i) {
        if (!s_devices[i].used) {
            entry = &s_devices[i];
            break;
        }
    }
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t speed = speed_hz == 0U ? SENSOR_BUS_I2C_SPEED_HZ : speed_hz;
    i2c_master_bus_handle_t master_bus = i2c_bus_get_internal_bus_handle(bus);
    if (master_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed,
    };
    i2c_master_dev_handle_t master_device = NULL;
    const esp_err_t add_err = i2c_master_bus_add_device(
        master_bus, &config, &master_device);
    if (add_err != ESP_OK) {
        return add_err;
    }

    *entry = (sensor_bus_device_entry_t) {
        .bus_device = entry,
        .master_device = master_device,
        .address = address,
        .used = true,
    };
    *device = entry;
    return ESP_OK;
}

static void sensor_bus_clear_tca_cache(void)
{
    s_tca9548a = NULL;
    s_tca9548a_address = NULL_I2C_DEV_ADDR;
}

static esp_err_t sensor_bus_cleanup_owned_devices(void)
{
    esp_err_t err = sensor_bus_device_delete_locked(&s_tca9548a);
    sensor_bus_clear_tca_cache();

    const esp_err_t ina_err = sensor_bus_device_delete_locked(&s_ina3221);
    if (err == ESP_OK) {
        err = ina_err;
    }
    for (size_t i = 0U; i < SENSOR_BUS_DEVICE_LIMIT; ++i) {
        if (s_devices[i].used && s_devices[i].bus_device != NULL) {
            const i2c_bus_device_handle_t owned_device = s_devices[i].bus_device;
            i2c_bus_device_handle_t device = owned_device;
            const esp_err_t delete_err = sensor_bus_device_delete_locked(&device);
            if (err == ESP_OK) {
                err = delete_err;
            }
            if (delete_err == ESP_OK) {
                if (s_ina3221 == owned_device) {
                    s_ina3221 = NULL;
                }
                if (s_tca9548a == owned_device) {
                    sensor_bus_clear_tca_cache();
                }
            }
        }
    }
    return err;
}

static void sensor_bus_copy_task_name(
    char destination[SENSOR_BUS_TASK_NAME_SIZE],
    TaskHandle_t task)
{
    const char *name = task != NULL ? pcTaskGetName(task) : NULL;
    if (name == NULL || name[0] == '\0') {
        name = "unknown";
    }
    (void)strncpy(destination, name, SENSOR_BUS_TASK_NAME_SIZE - 1U);
    destination[SENSOR_BUS_TASK_NAME_SIZE - 1U] = '\0';
}

static esp_err_t sensor_bus_lock_with_wait(uint64_t *waited_for_mutex_us)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const int64_t wait_started_us = sensor_bus_lines_now_us();
    uint64_t waited_us = 0U;

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(
            s_lock, pdMS_TO_TICKS(SENSOR_BUS_MUTEX_WAIT_MS)) != pdTRUE) {
        const int64_t wait_finished_us = sensor_bus_lines_now_us();
        waited_us = wait_finished_us >= wait_started_us
            ? (uint64_t)(wait_finished_us - wait_started_us) : 0U;
        portENTER_CRITICAL(&s_owner_stats_lock);
        ++s_owner_stats.busy_count;
        s_owner_stats.last_waited_for_mutex_us = waited_us;
        portEXIT_CRITICAL(&s_owner_stats_lock);
        if (waited_for_mutex_us != NULL) {
            *waited_for_mutex_us = waited_us;
        }
        return SENSOR_BUS_ERR_MUTEX_BUSY;
    }

    const int64_t wait_finished_us = sensor_bus_lines_now_us();
    waited_us = wait_finished_us >= wait_started_us
        ? (uint64_t)(wait_finished_us - wait_started_us) : 0U;
    char task_name[SENSOR_BUS_TASK_NAME_SIZE];
    sensor_bus_copy_task_name(task_name, current_task);
    portENTER_CRITICAL(&s_owner_stats_lock);
    s_owner_stats.last_waited_for_mutex_us = waited_us;
    if (s_owner_task == current_task) {
        ++s_owner_depth;
    } else {
        s_owner_task = current_task;
        s_owner_depth = 1U;
        s_owner_started_us = wait_finished_us;
        (void)memcpy(s_owner_stats.holder_task, task_name, sizeof(task_name));
        s_owner_stats.holder_task[sizeof(s_owner_stats.holder_task) - 1U] = '\0';
    }
    portEXIT_CRITICAL(&s_owner_stats_lock);
    if (waited_for_mutex_us != NULL) {
        *waited_for_mutex_us = waited_us;
    }
    return ESP_OK;
}

static void sensor_bus_unlock(void)
{
    if (s_lock != NULL) {
        const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
        const int64_t now_us = sensor_bus_lines_now_us();
        portENTER_CRITICAL(&s_owner_stats_lock);
        if (s_owner_task == current_task && s_owner_depth > 0U) {
            --s_owner_depth;
            if (s_owner_depth == 0U) {
                s_owner_stats.last_hold_us = now_us >= s_owner_started_us
                    ? (uint64_t)(now_us - s_owner_started_us) : 0U;
                s_owner_task = NULL;
                s_owner_started_us = 0;
            }
        }
        portEXIT_CRITICAL(&s_owner_stats_lock);
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

static bool sensor_bus_owner_budget_blocks(
    const sensor_bus_device_entry_t *entry,
    bool close_write,
    bool *close_read)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const bool tca_device = entry != NULL && entry->address == s_tca9548a_address;
    const int64_t now_us = sensor_bus_lines_now_us();
    bool blocked = false;
    if (close_read != NULL) {
        *close_read = false;
    }

    portENTER_CRITICAL(&s_owner_stats_lock);
    if (s_owner_session_depth != 0U && s_owner_task == current_task) {
        if (s_owner_hold_budget_exceeded) {
            if (close_write && tca_device) {
                s_owner_close_in_progress = true;
            } else {
                *close_read = tca_device && s_owner_close_in_progress;
                blocked = !*close_read;
            }
        } else if (close_write && tca_device) {
            const uint64_t elapsed_us = now_us >= s_owner_session_started_us
                ? (uint64_t)(now_us - s_owner_session_started_us) : 0U;
            if (elapsed_us + (uint64_t)SENSOR_BUS_MUX_CLOSE_RESERVE_MS * 1000U >
                (uint64_t)SENSOR_BUS_OWNER_MAX_HOLD_MS * 1000U) {
                s_owner_hold_budget_exceeded = true;
            }
            s_owner_close_in_progress = true;
        } else if (s_owner_close_in_progress && tca_device) {
            /* The readback is the second half of the mux-close operation. */
            *close_read = true;
        } else {
            const uint64_t elapsed_us = now_us >= s_owner_session_started_us
                ? (uint64_t)(now_us - s_owner_session_started_us) : 0U;
            const uint64_t next_step_us =
                (uint64_t)(SENSOR_BUS_TRANSACTION_TIMEOUT_MS + SENSOR_BUS_MUX_CLOSE_RESERVE_MS) * 1000U;
            if (elapsed_us + next_step_us >
                (uint64_t)SENSOR_BUS_OWNER_MAX_HOLD_MS * 1000U) {
                s_owner_hold_budget_exceeded = true;
                blocked = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_owner_stats_lock);
    return blocked;
}

static esp_err_t sensor_bus_owner_budget_after_transaction(
    const sensor_bus_device_entry_t *entry,
    bool close_write,
    bool close_read,
    esp_err_t err)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const bool tca_device = entry != NULL && entry->address == s_tca9548a_address;
    const int64_t now_us = sensor_bus_lines_now_us();
    bool exceeded = false;

    portENTER_CRITICAL(&s_owner_stats_lock);
    if (s_owner_session_depth != 0U && s_owner_task == current_task) {
        if (close_read && tca_device) {
            s_owner_close_in_progress = false;
        } else if (!close_write) {
            const uint64_t elapsed_us = now_us >= s_owner_session_started_us
                ? (uint64_t)(now_us - s_owner_session_started_us) : 0U;
            if (elapsed_us + (uint64_t)SENSOR_BUS_MUX_CLOSE_RESERVE_MS * 1000U >
                (uint64_t)SENSOR_BUS_OWNER_MAX_HOLD_MS * 1000U) {
                s_owner_hold_budget_exceeded = true;
            }
        }
        exceeded = s_owner_hold_budget_exceeded;
    }
    portEXIT_CRITICAL(&s_owner_stats_lock);

    if (exceeded && !close_write && !close_read) {
        return SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
    }
    return err;
}

static esp_err_t sensor_bus_lock(void)
{
    uint64_t waited_us = 0U;
    const esp_err_t err = sensor_bus_lock_with_wait(&waited_us);
    if (err == SENSOR_BUS_ERR_MUTEX_BUSY) {
        sensor_bus_owner_stats_t stats = {0};
        (void)sensor_bus_get_owner_stats(&stats);
        ESP_LOGW(TAG,
                 "I2C_TIMEOUT_DIAG src=bus_owner addr=0x00 dev=unknown ch=direct err=mutex_busy waited_for_mutex_us=%llu transfer_us=0 holder_task=%s recover_attempts=%lu",
                 (unsigned long long)waited_us,
                 stats.holder_task[0] != '\0' ? stats.holder_task : "unknown",
                 (unsigned long)stats.recover_attempts);
    }
    return err;
}

static const char *sensor_bus_default_diag_source(
    uint8_t address,
    bool is_probe)
{
    if (address == 0x38U || address == 0x76U || address == 0x77U) {
        return "main_barometer";
    }
    if (address == 0x40U) {
        return "ina3221";
    }
    if (address >= 0x30U && address <= 0x32U) {
        return "nrf_topology";
    }
    if (address >= 0x70U && address <= 0x75U) {
        return s_tca_diag_source;
    }
    return is_probe ? "probe" : "bus";
}

static sensor_bus_diag_result_t sensor_bus_diag_result_class(
    esp_err_t err,
    bool hardware_timeout_mapping,
    bool invalid_state_nack)
{
    if (err == ESP_OK) {
        return SENSOR_BUS_DIAG_RESULT_OK;
    }
    if (invalid_state_nack) {
        return SENSOR_BUS_DIAG_RESULT_NACK_INVALID_STATE;
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_FAIL) {
        return SENSOR_BUS_DIAG_RESULT_NACK;
    }
    if (err == ESP_ERR_TIMEOUT || hardware_timeout_mapping) {
        return SENSOR_BUS_DIAG_RESULT_TIMEOUT;
    }
    return SENSOR_BUS_DIAG_RESULT_OTHER;
}

static void sensor_bus_record_result(
    uint8_t address,
    const char *source,
    int channel,
    esp_err_t err,
    bool hardware_timeout_mapping,
    bool invalid_state_nack)
{
    const char *caller = source != NULL ? source : "bus";
    const sensor_bus_diag_result_t result = sensor_bus_diag_result_class(
        err, hardware_timeout_mapping, invalid_state_nack);
    const bool failed = result != SENSOR_BUS_DIAG_RESULT_OK;
    bool sda_high = false;
    bool scl_high = false;
    if (failed) {
        sensor_bus_lines_read(&sda_high, &scl_high);
    }

    const uint32_t now_ms =
        (uint32_t)(sensor_bus_lines_now_us() / INT64_C(1000));
    portENTER_CRITICAL(&s_diag_lock);
    (void)sensor_bus_diag_record_result(
        &s_diag_stats, now_ms, address, channel, (int)err, result,
        failed ? (sda_high ? 1 : 0) : -1,
        failed ? (scl_high ? 1 : 0) : -1,
        caller);
    const bool log_timeout = result == SENSOR_BUS_DIAG_RESULT_TIMEOUT &&
        sensor_bus_diag_rate_limit_allow(&s_diag_state, now_ms, caller);
    portEXIT_CRITICAL(&s_diag_lock);

    if (log_timeout) {
        char line[192];
        if (sensor_bus_diag_format(
                line, sizeof(line), caller, address, channel,
                esp_err_to_name(err),
                failed ? (sda_high ? 1 : 0) : -1,
                failed ? (scl_high ? 1 : 0) : -1)) {
            ESP_LOGW(TAG, "%s", line);
        }
    }
}

esp_err_t sensor_bus_lock_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sensor_bus_owner_acquire(void)
{
    const esp_err_t err = sensor_bus_lock();
    if (err != ESP_OK) {
        return err;
    }
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const int64_t now_us = sensor_bus_lines_now_us();
    portENTER_CRITICAL(&s_owner_stats_lock);
    if (s_owner_task == current_task) {
        if (s_owner_session_depth == 0U) {
            s_owner_session_started_us = now_us;
            s_owner_hold_budget_exceeded = false;
            s_owner_close_in_progress = false;
        }
        ++s_owner_session_depth;
    }
    portEXIT_CRITICAL(&s_owner_stats_lock);
    return ESP_OK;
}

void sensor_bus_owner_release(void)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const int64_t now_us = sensor_bus_lines_now_us();
    bool outermost = false;
    bool budget_exceeded = false;
    sensor_bus_owner_stats_t released_stats = {0};
    portENTER_CRITICAL(&s_owner_stats_lock);
    outermost = s_owner_task == current_task && s_owner_session_depth == 1U;
    if (outermost) {
        released_stats = s_owner_stats;
        released_stats.last_hold_us = now_us >= s_owner_started_us
            ? (uint64_t)(now_us - s_owner_started_us) : 0U;
        budget_exceeded = s_owner_hold_budget_exceeded ||
            released_stats.last_hold_us > (uint64_t)SENSOR_BUS_OWNER_MAX_HOLD_MS * 1000U;
        s_owner_session_depth = 0U;
        s_owner_session_started_us = 0;
        s_owner_hold_budget_exceeded = false;
        s_owner_close_in_progress = false;
    } else if (s_owner_task == current_task && s_owner_session_depth > 1U) {
        --s_owner_session_depth;
    }
    portEXIT_CRITICAL(&s_owner_stats_lock);
    sensor_bus_unlock();
    if (outermost) {
        if (budget_exceeded) {
            ESP_LOGW(TAG,
                     "TCA_HOLD_BUDGET_EXCEEDED hold_ms=%llu max_hold_ms=%u",
                     (unsigned long long)(released_stats.last_hold_us / 1000U),
                     (unsigned)SENSOR_BUS_OWNER_MAX_HOLD_MS);
        }
        ESP_LOGI(TAG,
                 "I2C_BUS_OWNER owner=%s wait_ms=%llu hold_ms=%llu busy_count=%lu",
                 released_stats.holder_task[0] != '\0'
                     ? released_stats.holder_task : "unknown",
                 (unsigned long long)(released_stats.last_waited_for_mutex_us / 1000U),
                 (unsigned long long)(released_stats.last_hold_us / 1000U),
                 (unsigned long)released_stats.busy_count);
    }
}

bool sensor_bus_owner_hold_budget_exceeded(void)
{
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    bool exceeded;
    portENTER_CRITICAL(&s_owner_stats_lock);
    exceeded = s_owner_task == current_task && s_owner_session_depth != 0U &&
        s_owner_hold_budget_exceeded;
    portEXIT_CRITICAL(&s_owner_stats_lock);
    return exceeded;
}

esp_err_t sensor_bus_get_owner_stats(sensor_bus_owner_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_owner_stats_lock);
    *stats = s_owner_stats;
    portEXIT_CRITICAL(&s_owner_stats_lock);
    return ESP_OK;
}

/* Caller holds s_lock. */
static void sensor_bus_record_recovery(i2c_line_result_t result, int64_t now_us)
{
    ++s_recovery.attempts;
    portENTER_CRITICAL(&s_owner_stats_lock);
    s_owner_stats.recover_attempts = s_recovery.attempts;
    portEXIT_CRITICAL(&s_owner_stats_lock);
    s_recovery.last_result = result;
    s_recovery.last_attempt_us = now_us;
    if (result == I2C_LINES_RECOVERED) {
        ++s_recovery.recoveries;
    }
    if (result == I2C_LINES_SDA_STUCK || result == I2C_LINES_SCL_STUCK) {
        ++s_recovery.failures;
        ++s_recovery.consecutive_failures;
    } else {
        s_recovery.consecutive_failures = 0U;
    }
}

/* Caller holds s_lock and no I2C driver owns the pins. */
static i2c_line_result_t sensor_bus_bitbang_clear_locked(void)
{
    i2c_line_io_t io;
    if (sensor_bus_lines_bitbang_io(&io) != ESP_OK) {
        return I2C_LINES_SCL_STUCK;
    }
    return i2c_line_recover(&io).result;
}

/* Caller holds s_lock and I2C0 exists. */
static void sensor_bus_sample_live_lines(bool *sda_stuck, bool *scl_stuck)
{
    *sda_stuck = true;
    *scl_stuck = true;
    for (uint32_t i = 0U; i < SENSOR_BUS_STUCK_SAMPLES; ++i) {
        if (i > 0U) {
            sensor_bus_lines_delay_us(SENSOR_BUS_STUCK_SAMPLE_US);
        }
        bool sda_high = false;
        bool scl_high = false;
        sensor_bus_lines_read(&sda_high, &scl_high);
        *sda_stuck = *sda_stuck && !sda_high;
        *scl_stuck = *scl_stuck && !scl_high;
    }
}

/* Caller holds s_lock and I2C0 exists. */
static i2c_line_result_t sensor_bus_live_clear_locked(void)
{
    bool sda_stuck = false;
    bool scl_stuck = false;
    sensor_bus_sample_live_lines(&sda_stuck, &scl_stuck);
    if (scl_stuck) {
        return I2C_LINES_SCL_STUCK;  /* never clock against a held SCL */
    }
    if (!sda_stuck) {
        return I2C_LINES_IDLE;
    }
    if (sensor_bus_lines_controller_reset(s_bus) != ESP_OK) {
        return I2C_LINES_SDA_STUCK;
    }
    sensor_bus_sample_live_lines(&sda_stuck, &scl_stuck);
    if (scl_stuck) {
        return I2C_LINES_SCL_STUCK;
    }
    return sda_stuck ? I2C_LINES_SDA_STUCK : I2C_LINES_RECOVERED;
}

/* Caller holds s_lock and I2C0 exists. */
static esp_err_t sensor_bus_classify_locked(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel)
{
    if (transfer_err == ESP_FAIL) {
        sensor_bus_record_result(
            address, source, channel, ESP_ERR_NOT_FOUND, false, false);
        return ESP_ERR_NOT_FOUND;  /* legacy driver NACK */
    }
    if (transfer_err != ESP_ERR_INVALID_STATE) {
        sensor_bus_record_result(
            address, source, channel, transfer_err, false, false);
        return transfer_err;
    }
    const esp_err_t probe_err = sensor_bus_probe_address_raw(address, channel);
    if (probe_err == ESP_ERR_NOT_FOUND) {
        sensor_bus_record_result(
            address, source, channel, transfer_err, false, true);
        return ESP_ERR_NOT_FOUND;
    }
    /* ACK: the target is there and the transfer itself failed. */
    const esp_err_t classified_err = probe_err == ESP_OK ? transfer_err : probe_err;
    sensor_bus_record_result(
        address, source, channel, classified_err,
        probe_err == ESP_OK, false);
    return classified_err;
}

/* Caller holds s_lock. Confirm the writable TCA control register and leave
 * the mux in exactly the state it had before discovery. */
static bool sensor_bus_verify_tca_candidate_locked(
    i2c_bus_device_handle_t device,
    uint8_t address
)
{
    uint8_t original_mask = 0U;
    esp_err_t err = sensor_bus_read_byte_diagnostic(
        device, address, -1, false, 0U, &original_mask);
    sensor_bus_record_result(
        address, s_tca_diag_source, -1, err, false, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "I2C_DEVICE_WARN src=tca9548a addr=0x%02X dev=%s reason=initial_control_read_failed err=%s",
                 address,
                 sensor_bus_diag_device_name(address, s_tca_diag_source),
                 esp_err_to_name(err));
        return false;
    }

    const uint8_t test_mask = (uint8_t)(original_mask ^ 0x01U);
    err = sensor_bus_write_byte_diagnostic(
        device, address, -1, SENSOR_BUS_PROBE_PHASE_TCA_SELECT, test_mask);
    sensor_bus_record_result(
        address, s_tca_diag_source, -1, err, false, false);
    bool valid = err == ESP_OK;
    const char *reason = "control write failed";
    if (valid) {
        uint8_t readback = 0U;
        err = sensor_bus_read_byte_diagnostic(
            device, address, -1, true, test_mask, &readback);
        sensor_bus_record_result(
            address, s_tca_diag_source, -1, err, false, false);
        if (err != ESP_OK) {
            valid = false;
            reason = "control readback failed";
        } else if (readback != test_mask) {
            valid = false;
            reason = "control readback mismatch";
        }
    }

    const esp_err_t restore_err = sensor_bus_write_byte_diagnostic(
        device, address, -1, SENSOR_BUS_PROBE_PHASE_TCA_CLOSE,
        original_mask);
    sensor_bus_record_result(
        address, s_tca_diag_source, -1, restore_err, false, false);
    uint8_t restored_mask = 0U;
    const bool restore_read_attempted = restore_err == ESP_OK;
    const esp_err_t restore_read_err = restore_read_attempted
        ? sensor_bus_read_byte_diagnostic(
            device, address, -1, true, original_mask, &restored_mask)
        : restore_err;
    if (restore_read_attempted) {
        sensor_bus_record_result(
            address, s_tca_diag_source, -1, restore_read_err, false, false);
    }
    if (restore_read_err != ESP_OK || restored_mask != original_mask) {
        valid = false;
        reason = "control restore failed";
    }

    if (!valid) {
        ESP_LOGW(TAG,
                 "I2C_DEVICE_WARN src=tca9548a addr=0x%02X dev=%s reason=%s err=%s",
                 address,
                 sensor_bus_diag_device_name(address, s_tca_diag_source),
                 reason,
                 esp_err_to_name(err));
    }
    return valid;
}

/* Caller must already hold s_lock. */
static esp_err_t sensor_bus_init_locked(void)
{
    if (s_bus != NULL && s_ina3221 != NULL) {
        return ESP_OK;
    }

    (void)sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        (void)i2c_bus_delete(&s_bus);
    }

    /* MAX-243: a target reset mid-transfer can hold SDA low across our own
     * reboot; clear it while the pins are still free. A stuck line is only
     * recorded -- the bus is still created, and its transfers fail and are
     * reported by their consumers. */
    sensor_bus_record_recovery(
        sensor_bus_bitbang_clear_locked(), sensor_bus_lines_now_us()
    );

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_SENSOR_I2C_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BOARD_SENSOR_I2C_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_BUS_I2C_SPEED_HZ,
    };

    s_bus = i2c_bus_create(SENSOR_BUS_I2C_PORT, &config);
    if (s_bus == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "I2C_TIMEOUT_CONFIG mutex_wait_ms=%u transaction_timeout_ms=%u owner_max_hold_ms=%u",
             (unsigned)SENSOR_BUS_MUTEX_WAIT_MS,
             (unsigned)SENSOR_BUS_TRANSACTION_TIMEOUT_MS,
             (unsigned)SENSOR_BUS_OWNER_MAX_HOLD_MS);

#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    ESP_LOGI(TAG,
             "I2C_CONFIG sda_gpio=%d scl_gpio=%d speed_hz=%lu sda_pullup=%u scl_pullup=%u glitch_ignore_cnt=%u timeout_mutex_ms=%u timeout_transfer_ms=%u timeout_owner_hold_ms=%u probe_timeout_ms=%u",
             (int)config.sda_io_num,
             (int)config.scl_io_num,
             (unsigned long)config.master.clk_speed,
             config.sda_pullup_en == GPIO_PULLUP_ENABLE ? 1U : 0U,
             config.scl_pullup_en == GPIO_PULLUP_ENABLE ? 1U : 0U,
             (unsigned)SENSOR_BUS_I2C_GLITCH_IGNORE_COUNT,
             (unsigned)SENSOR_BUS_MUTEX_WAIT_MS,
             (unsigned)SENSOR_BUS_TRANSACTION_TIMEOUT_MS,
             (unsigned)SENSOR_BUS_OWNER_MAX_HOLD_MS,
             (unsigned)SENSOR_BUS_LINES_PROBE_TIMEOUT_MS);
#endif

    const esp_err_t device_err = sensor_bus_device_create_locked(
        s_bus, BOARD_INA3221_I2C_ADDRESS, SENSOR_BUS_I2C_SPEED_HZ, &s_ina3221);
    if (device_err != ESP_OK) {
        (void)i2c_bus_delete(&s_bus);
        return device_err;
    }

    return ESP_OK;
}

esp_err_t sensor_bus_init(void)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    const esp_err_t err = sensor_bus_init_locked();
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const esp_err_t err = sensor_bus_init_locked();
    if (err == ESP_OK) {
        handles->bus = s_bus;
        handles->ina3221 = s_ina3221;
    }

    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_get_tca9548a(i2c_bus_device_handle_t *device, uint8_t *address)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = NULL;
    if (address != NULL) {
        *address = NULL_I2C_DEV_ADDR;
    }

    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = sensor_bus_init_locked();
    if (err != ESP_OK) {
        sensor_bus_unlock();
        return err;
    }

    if (s_tca9548a != NULL) {
        *device = s_tca9548a;
        if (address != NULL) {
            *address = s_tca9548a_address;
        }
        sensor_bus_unlock();
        return ESP_OK;
    }

    i2c_bus_device_handle_t found = NULL;
    uint8_t found_address = NULL_I2C_DEV_ADDR;

    for (uint8_t candidate_address = SENSOR_BUS_TCA9548A_ADDR_MIN;
         candidate_address <= SENSOR_BUS_TCA9548A_ADDR_MAX;
         ++candidate_address) {
        /* BMP280 shares the main bus and may occupy either address. */
        if (candidate_address >= SENSOR_BUS_BMP280_ADDR_MIN &&
            candidate_address <= SENSOR_BUS_BMP280_ADDR_MAX) {
            continue;
        }

        const esp_err_t address_probe_err =
            sensor_bus_probe_address_raw(candidate_address, -1);
        sensor_bus_record_result(
            candidate_address, s_tca_diag_source, -1,
            address_probe_err, false, false);
        if (address_probe_err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (address_probe_err != ESP_OK) {
            err = address_probe_err;
            goto fail;
        }

        i2c_bus_device_handle_t candidate = NULL;
        err = sensor_bus_device_create_locked(
            s_bus, candidate_address, SENSOR_BUS_I2C_SPEED_HZ, &candidate);
        if (err != ESP_OK) {
            goto fail;
        }

        /* MAX-248: a NACK is ESP_ERR_NOT_FOUND after classification,
         * whatever code the backend used for it. */
        uint8_t response = 0U;
        const esp_err_t probe_err = sensor_bus_classify_locked(
            candidate_address,
            sensor_bus_read_byte_diagnostic(
                candidate, candidate_address, -1, false, 0U, &response),
            s_tca_diag_source,
            -1);

        if (probe_err == ESP_OK &&
            sensor_bus_verify_tca_candidate_locked(candidate, candidate_address)) {
            if (found != NULL) {
                const esp_err_t candidate_delete_err =
                    sensor_bus_device_delete_locked(&candidate);
                const esp_err_t found_delete_err =
                    sensor_bus_device_delete_locked(&found);
                sensor_bus_unlock();
                if (candidate_delete_err != ESP_OK) {
                    return candidate_delete_err;
                }
                if (found_delete_err != ESP_OK) {
                    return found_delete_err;
                }
                return ESP_ERR_INVALID_STATE;
            }
            found = candidate;
            found_address = candidate_address;
            continue;
        }

        const esp_err_t delete_err = sensor_bus_device_delete_locked(&candidate);
        if (delete_err != ESP_OK) {
            err = delete_err;
            goto fail;
        }
        if (probe_err != ESP_ERR_NOT_FOUND && probe_err != ESP_OK) {
            err = probe_err;
            goto fail;
        }
    }

    if (found == NULL) {
        sensor_bus_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    s_tca9548a = found;
    s_tca9548a_address = found_address;
    *device = s_tca9548a;
    if (address != NULL) {
        *address = s_tca9548a_address;
    }
    sensor_bus_unlock();
    return ESP_OK;

fail:
    if (found != NULL) {
        const esp_err_t cleanup_err = sensor_bus_device_delete_locked(&found);
        sensor_bus_unlock();
        if (cleanup_err != ESP_OK) {
            return cleanup_err;
        }
        return err;
    }
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_device_create(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed_hz,
    i2c_bus_device_handle_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = NULL;
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    const esp_err_t err = sensor_bus_device_create_locked(
        bus, address, speed_hz, device);
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_device_delete(i2c_bus_device_handle_t *device)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    const esp_err_t err = sensor_bus_device_delete_locked(device);
    sensor_bus_unlock();
    return err;
}

static sensor_bus_device_entry_t *sensor_bus_find_device_locked(
    i2c_bus_device_handle_t device)
{
    for (size_t i = 0U; i < SENSOR_BUS_DEVICE_LIMIT; ++i) {
        if (s_devices[i].used && s_devices[i].bus_device == device) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static void sensor_bus_log_timeout_diag(
    const sensor_bus_device_entry_t *entry,
    uint64_t waited_for_mutex_us,
    uint64_t transfer_us,
    bool mutex_busy)
{
    sensor_bus_owner_stats_t stats = {0};
    char holder_name[SENSOR_BUS_TASK_NAME_SIZE];
    const uint8_t address = entry != NULL ? entry->address : 0U;
    const char *source = address != 0U
        ? sensor_bus_default_diag_source(address, false) : "bus_owner";
    (void)sensor_bus_get_owner_stats(&stats);
    if (mutex_busy) {
        (void)strncpy(holder_name, stats.holder_task, sizeof(holder_name) - 1U);
        holder_name[sizeof(holder_name) - 1U] = '\0';
        if (holder_name[0] == '\0') {
            (void)strcpy(holder_name, "unknown");
        }
    } else {
        sensor_bus_copy_task_name(holder_name, xTaskGetCurrentTaskHandle());
    }
    ESP_LOGW(TAG,
             "I2C_TIMEOUT_DIAG src=%s addr=0x%02X dev=%s ch=direct err=%s waited_for_mutex_us=%llu transfer_us=%llu holder_task=%s recover_attempts=%lu",
             source,
             (unsigned)address,
             sensor_bus_diag_device_name(address, source),
             mutex_busy ? "mutex_busy" : esp_err_to_name(ESP_ERR_TIMEOUT),
             (unsigned long long)waited_for_mutex_us,
             (unsigned long long)transfer_us,
             holder_name,
             (unsigned long)stats.recover_attempts);
}

static esp_err_t sensor_bus_find_native_device(
    i2c_bus_device_handle_t device,
    sensor_bus_device_entry_t **entry)
{
    if (device == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *entry = sensor_bus_find_device_locked(device);
    return *entry != NULL && (*entry)->master_device != NULL
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t sensor_bus_read_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data)
{
    if (data == NULL || data_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t waited_us = 0U;
    const esp_err_t lock_err = sensor_bus_lock_with_wait(&waited_us);
    if (lock_err != ESP_OK) {
        if (lock_err == SENSOR_BUS_ERR_MUTEX_BUSY) {
            sensor_bus_log_timeout_diag(NULL, waited_us, 0U, true);
        }
        return lock_err;
    }

    sensor_bus_device_entry_t *entry = NULL;
    esp_err_t err = sensor_bus_find_native_device(device, &entry);
    if (err == ESP_OK) {
        bool close_read = false;
        if (sensor_bus_owner_budget_blocks(entry, false, &close_read)) {
            err = SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
        } else {
        const int64_t transfer_started_us = sensor_bus_lines_now_us();
        if (mem_address == NULL_I2C_MEM_ADDR) {
            err = i2c_master_receive(
                entry->master_device, data, data_len,
                SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
        } else {
            err = i2c_master_transmit_receive(
                entry->master_device, &mem_address, 1U, data, data_len,
                SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
        }
        const int64_t transfer_finished_us = sensor_bus_lines_now_us();
        if (err == ESP_ERR_TIMEOUT) {
            sensor_bus_log_timeout_diag(
                entry, waited_us,
                transfer_finished_us >= transfer_started_us
                    ? (uint64_t)(transfer_finished_us - transfer_started_us) : 0U,
                false);
        }
        err = sensor_bus_owner_budget_after_transaction(
            entry, false, close_read, err);
        }
    }
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data)
{
    return sensor_bus_read_bytes(device, mem_address, 1U, data);
}

esp_err_t sensor_bus_write_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    const uint8_t *data)
{
    if (data_len > 0U && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool has_mem_address = mem_address != NULL_I2C_MEM_ADDR;
    if ((data_len == 0U && !has_mem_address) ||
        (has_mem_address && data_len == SIZE_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t stack_buffer[SENSOR_BUS_WRITE_STACK_BYTES];
    const size_t tx_len = data_len + (has_mem_address ? 1U : 0U);
    uint8_t *tx = tx_len <= sizeof(stack_buffer)
        ? stack_buffer : malloc(tx_len);
    if (tx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (has_mem_address) {
        tx[0] = mem_address;
        if (data_len > 0U) {
            memcpy(&tx[1], data, data_len);
        }
    } else if (data_len > 0U) {
        memcpy(tx, data, data_len);
    }

    uint64_t waited_us = 0U;
    const esp_err_t lock_err = sensor_bus_lock_with_wait(&waited_us);
    if (lock_err != ESP_OK) {
        if (tx != stack_buffer) {
            free(tx);
        }
        if (lock_err == SENSOR_BUS_ERR_MUTEX_BUSY) {
            sensor_bus_log_timeout_diag(NULL, waited_us, 0U, true);
        }
        return lock_err;
    }

    sensor_bus_device_entry_t *entry = NULL;
    esp_err_t err = sensor_bus_find_native_device(device, &entry);
    if (err == ESP_OK) {
        const bool close_write = entry->address == s_tca9548a_address &&
            !has_mem_address && data_len == 1U && data[0] == 0U;
        bool close_read = false;
        if (sensor_bus_owner_budget_blocks(entry, close_write, &close_read)) {
            err = SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
        } else {
        const int64_t transfer_started_us = sensor_bus_lines_now_us();
        err = i2c_master_transmit(
            entry->master_device, tx, tx_len,
            SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
        const int64_t transfer_finished_us = sensor_bus_lines_now_us();
        if (err == ESP_ERR_TIMEOUT) {
            sensor_bus_log_timeout_diag(
                entry, waited_us,
                transfer_finished_us >= transfer_started_us
                    ? (uint64_t)(transfer_finished_us - transfer_started_us) : 0U,
                false);
        }
        err = sensor_bus_owner_budget_after_transaction(
            entry, close_write, close_read, err);
        }
    }
    sensor_bus_unlock();
    if (tx != stack_buffer) {
        free(tx);
    }
    return err;
}

esp_err_t sensor_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data)
{
    return sensor_bus_write_bytes(device, mem_address, 1U, &data);
}

esp_err_t sensor_bus_probe_address_with_context(
    uint8_t address,
    const char *source,
    int channel)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = sensor_bus_init_locked();
    if (err == ESP_OK) {
        err = sensor_bus_probe_address_raw(address, channel);
        if (err == ESP_OK || source == NULL ||
            strcmp(source, s_nrf_diag_source) != 0) {
            sensor_bus_record_result(
                address, source, channel, err, false, false);
        }
    }
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_probe_address(uint8_t address)
{
    return sensor_bus_probe_address_with_context(
        address, sensor_bus_default_diag_source(address, true), -1);
}

esp_err_t sensor_bus_classify_transfer_error_with_context(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel)
{
    if (transfer_err == ESP_OK) {
        sensor_bus_record_result(address, source, channel, ESP_OK, false, false);
        return ESP_OK;
    }
    if (transfer_err != ESP_ERR_INVALID_STATE) {
        const esp_err_t classified_err = transfer_err == ESP_FAIL
            ? ESP_ERR_NOT_FOUND : transfer_err;
        sensor_bus_record_result(
            address, source, channel, classified_err, false, false);
        return classified_err;
    }
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return transfer_err;
    }
    const esp_err_t err = s_bus != NULL
        ? sensor_bus_classify_locked(address, transfer_err, source, channel)
        : transfer_err;
    if (s_bus == NULL) {
        sensor_bus_record_result(
            address, source, channel, transfer_err, false, false);
    }
    sensor_bus_unlock();
    return err;
}

esp_err_t sensor_bus_classify_transfer_error(uint8_t address, esp_err_t transfer_err)
{
    return sensor_bus_classify_transfer_error_with_context(
        address, transfer_err, sensor_bus_default_diag_source(address, false), -1);
}

void sensor_bus_record_transfer_result_with_context(
    uint8_t address,
    esp_err_t transfer_err,
    const char *source,
    int channel)
{
    sensor_bus_record_result(
        address, source, channel, transfer_err, false, false);
}

esp_err_t sensor_bus_recover(i2c_line_result_t *result)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const int64_t now_us = sensor_bus_lines_now_us();
    if (!i2c_line_recovery_backoff_allows(
            now_us, s_recovery.last_attempt_us, s_recovery.consecutive_failures)) {
        sensor_bus_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_line_result_t line_result = s_bus != NULL
        ? sensor_bus_live_clear_locked()
        : sensor_bus_bitbang_clear_locked();
    sensor_bus_record_recovery(line_result, now_us);
    sensor_bus_unlock();

    if (result != NULL) {
        *result = line_result;
    }
    return line_result == I2C_LINES_IDLE || line_result == I2C_LINES_RECOVERED
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

esp_err_t sensor_bus_get_recovery_status(sensor_bus_recovery_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    memcpy(status, &s_recovery, sizeof(*status));
    sensor_bus_unlock();
    return ESP_OK;
}

static const char *sensor_bus_diag_error_name(int err, void *context)
{
    (void)context;
    return esp_err_to_name((esp_err_t)err);
}

static void sensor_bus_diag_print_line(const char *line, void *context)
{
    (void)context;
    ESP_LOGW(TAG, "%s", line);
}

void sensor_bus_log_diag_summary(uint32_t now_ms)
{
    sensor_bus_diag_summary_t summary;
    portENTER_CRITICAL(&s_diag_lock);
    const bool should_emit = sensor_bus_diag_summary_capture(
        &s_diag_stats, now_ms, &summary);
    portEXIT_CRITICAL(&s_diag_lock);
    if (!should_emit) {
        return;
    }

    sensor_bus_recovery_status_t recovery = { 0 };
    (void)sensor_bus_get_recovery_status(&recovery);
    bool sda_high = false;
    bool scl_high = false;
    sensor_bus_lines_read(&sda_high, &scl_high);
    (void)sensor_bus_diag_emit_summary(
        &summary,
        recovery.attempts,
        recovery.recoveries,
        recovery.failures,
        recovery.consecutive_failures,
        sda_high ? 1 : 0,
        scl_high ? 1 : 0,
        sensor_bus_diag_error_name,
        sensor_bus_diag_print_line,
        NULL);
}

esp_err_t sensor_bus_deinit(void)
{
    const esp_err_t lock_err = sensor_bus_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = sensor_bus_cleanup_owned_devices();
    if (s_bus != NULL) {
        const esp_err_t bus_err = i2c_bus_delete(&s_bus);
        if (err == ESP_OK) {
            err = bus_err;
        }
    }

    sensor_bus_unlock();
    return err;
}
