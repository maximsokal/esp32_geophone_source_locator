#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "app_events.h"
#include "board_config.h"
#include "device_status.h"
#include "ina3221_monitor.h"
#include "main_barometer.h"
#include "nrf_topology_main_barometer.h"
#include "sensor_bus.h"
#include "sensor_bus_lines.h"
#include "sensor_bus_lines_fake.h"

/*
 * End-to-end host test of the MAIN barometer, the INA3221 power monitor and the
 * shared I2C bus, all of them PRODUCTION code:
 *
 *   sensor_bus.c            the real bus owner / mutex / quiet lease / error classifier
 *   ina3221_monitor.c       the real worker TASK (its own thread here), driver and policy
 *   main_barometer.c        the real BMP280 state machine and height-reference capture
 *   nrf_topology_main_barometer.c  the real publisher into device_status
 *
 * Only the silicon is modelled: a register-level BMP280 (0x76, continuous
 * conversions, status "measuring" bit, data registers that change when a
 * conversion ends) and a register-level INA3221 (0x40) sit behind the I2C
 * controller API, so every byte the firmware reads or writes goes through the
 * real driver stack. A shared fake clock drives everything.
 *
 * Scheduling is deterministic lock-step: the INA3221 worker is a real thread,
 * but only one thread runs at any moment. The test thread plays the nRF
 * topology task (BMP280 polling, REMOTE bus sessions, the BLE quiet lease);
 * the worker runs whenever its ulTaskNotifyTake() timeout or a notification
 * is due. That makes bus contention, faults, lease start/end and shutdown
 * reproducible without wall-clock sleeps.
 *
 * Not covered here (see workflow_service_test.c): the placement accumulator of
 * the REMOTE nodes. REMOTE refresh traffic is only modelled as bus load.
 */

#define TRANSFER_US 1200U
#define INA_ADDR 0x40U
#define BMP_ADDR 0x76U
#define REMOTE_ADDR 0x30U
#define BMP_CYCLE_US INT64_C(110000)
#define BMP_MEASURE_US INT64_C(45000)

/* ------------------------------------------------------------------------- */
/* Clock, logs                                                                */
/* ------------------------------------------------------------------------- */

int64_t esp_timer_get_time(void)
{
    return sensor_bus_lines_now_us();
}

#define NOW_US() sensor_bus_lines_now_us()
#define NOW_MS() ((uint64_t)(NOW_US() / 1000))

static void clock_advance_to(int64_t target_us)
{
    const int64_t now_us = NOW_US();
    if (target_us > now_us) {
        assert(target_us - now_us < INT64_C(4000000000));
        sensor_bus_lines_delay_us((uint32_t)(target_us - now_us));
    }
}

#define LOG_CAPACITY 60000U
#define LOG_LINE 200U
static char (*s_logs)[LOG_LINE];
static size_t s_log_count;

static void log_capture(const char *line)
{
    if (s_log_count < LOG_CAPACITY) {
        (void)strncpy(s_logs[s_log_count], line, LOG_LINE - 1U);
        s_logs[s_log_count][LOG_LINE - 1U] = '\0';
        ++s_log_count;
    }
}

static size_t log_count_since(size_t mark, const char *needle)
{
    size_t count = 0U;
    for (size_t i = mark; i < s_log_count; ++i) {
        if (strstr(s_logs[i], needle) != NULL) ++count;
    }
    return count;
}

static const char *log_find_since(size_t mark, const char *needle)
{
    for (size_t i = mark; i < s_log_count; ++i) {
        if (strstr(s_logs[i], needle) != NULL) return s_logs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Lock-step scheduler: one real worker thread, never two running at once     */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
typedef HANDLE baton_t;
typedef HANDLE thread_handle_t;
static void baton_init(baton_t *baton)
{
    *baton = CreateSemaphoreA(NULL, 0, 1, NULL);
    assert(*baton != NULL);
}
static void baton_post(baton_t *baton) { assert(ReleaseSemaphore(*baton, 1, NULL)); }
static void baton_wait(baton_t *baton) { assert(WaitForSingleObject(*baton, INFINITE) == WAIT_OBJECT_0); }
static DWORD WINAPI worker_thread_main(LPVOID unused);
static thread_handle_t thread_start(void)
{
    HANDLE thread = CreateThread(NULL, 0, worker_thread_main, NULL, 0, NULL);
    assert(thread != NULL);
    return thread;
}
static void thread_join(thread_handle_t thread)
{
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
}
#else
typedef sem_t baton_t;
typedef pthread_t thread_handle_t;
static void baton_init(baton_t *baton) { assert(sem_init(baton, 0, 0U) == 0); }
static void baton_post(baton_t *baton) { assert(sem_post(baton) == 0); }
static void baton_wait(baton_t *baton)
{
    while (sem_wait(baton) != 0) assert(errno == EINTR);
}
static void *worker_thread_main(void *unused);
static thread_handle_t thread_start(void)
{
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker_thread_main, NULL) == 0);
    return thread;
}
static void thread_join(thread_handle_t thread) { assert(pthread_join(thread, NULL) == 0); }
#endif

static int s_main_token;
static int s_ina_token;
static void *s_current = &s_main_token;

static int task_index(void) { return s_current == &s_ina_token ? 1 : 0; }

typedef struct {
    baton_t to_worker;
    baton_t to_main;
    thread_handle_t thread;
    TaskFunction_t entry;
    void *argument;
    bool created;
    bool parked;
    bool finished;
    int64_t wake_at_us;
    unsigned notifications;
    unsigned resumes;
} sim_worker_t;

static sim_worker_t s_ina;

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task, const char *name, uint32_t stack_depth, void *argument,
    UBaseType_t priority, TaskHandle_t *created_task, BaseType_t core_id)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)core_id;
    assert(!s_ina.created);
    memset(&s_ina, 0, sizeof(s_ina));
    baton_init(&s_ina.to_worker);
    baton_init(&s_ina.to_main);
    s_ina.entry = task;
    s_ina.argument = argument;
    s_ina.created = true;
    s_ina.parked = true;
    s_ina.wake_at_us = NOW_US();
    s_ina.thread = thread_start();
    if (created_task != NULL) *created_task = &s_ina_token;
    return pdPASS;
}

#ifdef _WIN32
static DWORD WINAPI worker_thread_main(LPVOID unused)
#else
static void *worker_thread_main(void *unused)
#endif
{
    (void)unused;
    baton_wait(&s_ina.to_worker);
    s_ina.entry(s_ina.argument);
    s_ina.finished = true;
    baton_post(&s_ina.to_main);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_current; }

const char *pcTaskGetName(TaskHandle_t task)
{
    return task == &s_ina_token ? "ina3221" : "nrf_topology";
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &s_ina_token);
    ++s_ina.notifications;
    return pdPASS;
}

/* Called by the worker: park until the scheduler resumes it. */
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    assert(s_current == &s_ina_token);
    assert(ticks_to_wait != portMAX_DELAY);
    if (s_ina.notifications == 0U) {
        s_ina.wake_at_us = NOW_US() + (int64_t)ticks_to_wait * INT64_C(1000000) / (int64_t)configTICK_RATE_HZ;
        s_ina.parked = true;
        baton_post(&s_ina.to_main);
        baton_wait(&s_ina.to_worker);
    }
    const unsigned taken = s_ina.notifications;
    if (clear_on_exit) {
        s_ina.notifications = 0U;
    } else if (taken > 0U) {
        --s_ina.notifications;
    }
    return taken;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }

/* ------------------------------------------------------------------------- */
/* Bus mutex: recursive, never blocks (the holder cannot run in lock-step)    */
/* ------------------------------------------------------------------------- */

struct test_semaphore {
    void *owner;
    int depth;
};
static struct test_semaphore s_lock;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_lock; }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return &s_lock; }

static BaseType_t lock_take(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    assert(semaphore == &s_lock);
    if (semaphore->owner == NULL || semaphore->owner == s_current) {
        semaphore->owner = s_current;
        ++semaphore->depth;
        return pdTRUE;
    }
    /* The other task holds the bus and is parked, so waiting can only time out. */
    assert(ticks_to_wait != portMAX_DELAY);
    sensor_bus_lines_delay_us((uint32_t)((uint64_t)ticks_to_wait * 1000U / configTICK_RATE_HZ) * 1000U);
    return pdFALSE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    return lock_take(semaphore, ticks_to_wait);
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    return lock_take(semaphore, ticks_to_wait);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_lock && semaphore->owner == s_current && semaphore->depth > 0);
    if (--semaphore->depth == 0) semaphore->owner = NULL;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore) { return xSemaphoreGive(semaphore); }

/* ------------------------------------------------------------------------- */
/* Device status and application events (recorders)                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t sample_ms;
    bool valid;
    device_main_height_reference_state_t reference;
    uint16_t generation;
    int32_t relative_z_mm;
    uint32_t pressure_pa;
    int64_t at_us;
} main_record_t;

#define MAIN_RECORDS 24000U
static main_record_t s_main_records[MAIN_RECORDS];
static size_t s_main_record_count;

esp_err_t device_status_set_main(const device_main_status_t *main_status)
{
    assert(main_status != NULL);
    if (s_main_record_count < MAIN_RECORDS) {
        main_record_t *record = &s_main_records[s_main_record_count++];
        record->sample_ms = main_status->barometer_sample_ms;
        record->valid = main_status->bmp280_valid;
        record->reference = main_status->height_reference.state;
        record->generation = main_status->height_reference.generation;
        record->relative_z_mm = main_status->height_reference.relative_z_mm;
        record->pressure_pa = main_status->pressure_pa;
        record->at_us = NOW_US();
    }
    return ESP_OK;
}

static bool s_power_last_valid;
static unsigned s_power_valid_calls;
static unsigned s_power_invalid_calls;
static int64_t s_power_last_valid_at_us;

esp_err_t device_status_set_power(float voltage_v, float current_a, bool valid)
{
    (void)voltage_v;
    (void)current_a;
    s_power_last_valid = valid;
    if (valid) {
        ++s_power_valid_calls;
        s_power_last_valid_at_us = NOW_US();
    } else {
        ++s_power_invalid_calls;
    }
    return ESP_OK;
}

esp_err_t device_status_set_battery_soc_quality(device_battery_soc_quality_t quality)
{
    (void)quality;
    return ESP_OK;
}

esp_err_t device_status_set_power_state(
    device_power_state_t state, bool battery_full, device_low_battery_phase_t low_battery,
    uint32_t shutdown_in_s)
{
    (void)state;
    (void)battery_full;
    (void)low_battery;
    (void)shutdown_in_s;
    return ESP_OK;
}

esp_err_t device_status_set_power_channel_overcurrent(
    size_t channel_index, device_power_overcurrent_t overcurrent, bool latched)
{
    (void)channel_index;
    (void)overcurrent;
    (void)latched;
    return ESP_OK;
}

esp_err_t device_status_set_power_channel(size_t channel_index, float voltage_v, float current_a, bool valid)
{
    (void)channel_index;
    (void)voltage_v;
    (void)current_a;
    (void)valid;
    return ESP_OK;
}

esp_err_t device_status_set_power_battery(
    float battery_voltage_v, float battery_current_a, float battery_percent_estimate,
    bool battery_present, bool battery_percent_valid, bool usb_present, bool charging,
    device_power_source_t source)
{
    (void)battery_voltage_v;
    (void)battery_current_a;
    (void)battery_percent_estimate;
    (void)battery_present;
    (void)battery_percent_valid;
    (void)usb_present;
    (void)charging;
    (void)source;
    return ESP_OK;
}

static unsigned s_power_alerts;

esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value)
{
    (void)type;
    (void)value;
    ++s_power_alerts;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Silicon models behind the I2C controller                                   */
/* ------------------------------------------------------------------------- */

static unsigned s_transfers[2];       /* per task: 0 = topology/test thread, 1 = INA3221 worker */
static unsigned s_addr_transfers[128];
static int s_timeout_faults[128];     /* >0: that many transfers fail; <0: always */
static esp_err_t s_fault_code[128];   /* what they fail with; 0 = ESP_ERR_TIMEOUT. ESP_ERR_INVALID_STATE with an ACKing address is the IDF 5.5 "transfer failed" case */

/* INA3221 */
static uint16_t s_ina_config = 0x7127U;
static uint16_t s_ina_limits[6];
static uint16_t s_ina_bus_raw[3];
static uint16_t s_ina_shunt_raw[3];
static unsigned s_ina_measurement_register_reads;

static uint16_t ina_bus_raw_from_volts(float volts)
{
    return (uint16_t)((int16_t)(volts / 0.008f) * 8);
}

static uint16_t ina_shunt_raw_from_amps(float amps)
{
    return (uint16_t)((int16_t)(amps * 0.1f / 0.00004f) * 8);
}

static uint16_t ina_register(uint8_t reg)
{
    if (reg == 0x00U) return s_ina_config;
    if (reg >= 0x01U && reg <= 0x06U) {
        ++s_ina_measurement_register_reads;
        const size_t channel = (size_t)(reg - 1U) / 2U;
        return (reg & 1U) != 0U ? s_ina_shunt_raw[channel] : s_ina_bus_raw[channel];
    }
    if (reg >= 0x07U && reg <= 0x0CU) return s_ina_limits[reg - 0x07U];
    if (reg == 0x0FU) return 0U;
    if (reg == 0xFEU) return 0x5449U;
    if (reg == 0xFFU) return 0x3220U;
    return 0U;
}

static void ina_write(uint8_t reg, uint16_t value)
{
    if (reg == 0x00U) s_ina_config = value;
    else if (reg >= 0x07U && reg <= 0x0CU) s_ina_limits[reg - 0x07U] = value;
}

/* BMP280: data sheet calibration, so the production compensation runs on real numbers. */
static const uint16_t s_bmp_calibration[12] = {
    27504U, 26435U, (uint16_t)-1000, 36477U, (uint16_t)-10685, 3024U,
    2855U, 140U, (uint16_t)-7, 15500U, (uint16_t)-14600, 6000U,
};
static const int s_pressure_delta[8] = { 0, 3, -2, 1, -3, 2, -1, 0 };

static struct {
    uint8_t ctrl;
    uint8_t config;
    bool running;
    int64_t t0_us;
    uint32_t epoch;
    unsigned data_reads;
    unsigned duplicate_reads;
    unsigned forced_writes;
    uint64_t ids[16384];
    size_t id_count;
} s_bmp;

static bool bmp_measuring(int64_t now_us)
{
    return s_bmp.running && ((now_us - s_bmp.t0_us) % BMP_CYCLE_US) < BMP_MEASURE_US;
}

static bool bmp_held_sample(int64_t now_us, uint32_t *raw_pressure, uint64_t *id)
{
    if (!s_bmp.running || now_us - s_bmp.t0_us < BMP_MEASURE_US) return false;
    const int64_t k = (now_us - s_bmp.t0_us - BMP_MEASURE_US) / BMP_CYCLE_US;
    *raw_pressure = (uint32_t)(415148 + s_pressure_delta[k & 7]);
    *id = ((uint64_t)s_bmp.epoch << 32) | (uint64_t)k;
    return true;
}

static uint8_t bmp_byte(uint8_t reg, int64_t now_us, uint32_t raw_pressure, bool have_sample)
{
    if (reg >= 0x88U && reg <= 0x9FU) {
        const uint16_t word = s_bmp_calibration[(reg - 0x88U) / 2U];
        return (reg & 1U) == 0U ? (uint8_t)(word & 0xFFU) : (uint8_t)(word >> 8);
    }
    switch (reg) {
        case 0xD0U: return 0x58U;
        case 0xF3U: return bmp_measuring(now_us) ? 0x08U : 0x00U;
        case 0xF4U: return s_bmp.ctrl;
        case 0xF5U: return s_bmp.config;
        case 0xF7U: return have_sample ? (uint8_t)(raw_pressure >> 12) : 0x80U;
        case 0xF8U: return have_sample ? (uint8_t)((raw_pressure >> 4) & 0xFFU) : 0x00U;
        case 0xF9U: return have_sample ? (uint8_t)((raw_pressure & 0x0FU) << 4) : 0x00U;
        case 0xFAU: return have_sample ? (uint8_t)(519888U >> 12) : 0x80U;
        case 0xFBU: return have_sample ? (uint8_t)((519888U >> 4) & 0xFFU) : 0x00U;
        case 0xFCU: return have_sample ? (uint8_t)((519888U & 0x0FU) << 4) : 0x00U;
        default: return 0x00U;
    }
}

static void bmp_read(uint8_t reg, size_t length, uint8_t *out)
{
    const int64_t now_us = NOW_US();
    uint32_t raw_pressure = 0U;
    uint64_t id = 0U;
    const bool have_sample = bmp_held_sample(now_us, &raw_pressure, &id);
    if (reg == 0xF7U && length >= 6U) {
        ++s_bmp.data_reads;
        if (have_sample) {
            for (size_t i = 0U; i < s_bmp.id_count; ++i) {
                if (s_bmp.ids[i] == id) {
                    ++s_bmp.duplicate_reads;
                    break;
                }
            }
            if (s_bmp.id_count < sizeof(s_bmp.ids) / sizeof(s_bmp.ids[0])) {
                s_bmp.ids[s_bmp.id_count++] = id;
            }
        }
    }
    for (size_t i = 0U; i < length; ++i) {
        out[i] = bmp_byte((uint8_t)(reg + i), now_us, raw_pressure, have_sample);
    }
}

static void bmp_write(uint8_t reg, uint8_t value)
{
    const int64_t now_us = NOW_US();
    if (reg == 0xF5U) {
        s_bmp.config = value;
    } else if (reg == 0xF4U) {
        s_bmp.ctrl = value;
        const uint8_t mode = value & 0x03U;
        if (mode == 0x03U) {
            if (!s_bmp.running) {
                s_bmp.running = true;
                s_bmp.t0_us = now_us;
                ++s_bmp.epoch;
            }
        } else if (mode == 0U) {
            s_bmp.running = false;
        } else {
            ++s_bmp.forced_writes;
        }
    } else if (reg == 0xE0U && value == 0xB6U) {
        s_bmp.ctrl = 0U;
        s_bmp.config = 0U;
        s_bmp.running = false;
    }
}

/* I2C controller: the entry points sensor_bus.c drives */
typedef struct {
    uint8_t address;
    bool active;
} fake_master_device_t;

static int s_bus_token;
static int s_master_bus_token;
static uint8_t s_device_tokens[128];
static fake_master_device_t s_master_devices[16];

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    (void)port;
    (void)config;
    return &s_bus_token;
}

i2c_master_bus_handle_t i2c_bus_get_internal_bus_handle(void *bus)
{
    assert(bus == &s_bus_token);
    return &s_master_bus_token;
}

esp_err_t i2c_bus_delete(i2c_bus_handle_t *bus)
{
    assert(bus != NULL && *bus == &s_bus_token);
    *bus = NULL;
    return ESP_OK;
}

i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t speed_hz)
{
    (void)speed_hz;
    assert(bus == &s_bus_token && address < 128U);
    return &s_device_tokens[address];
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    assert(device != NULL && *device != NULL);
    *device = NULL;
    return ESP_OK;
}

esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data)
{
    (void)device;
    (void)mem_address;
    (void)data;
    return ESP_FAIL; /* only the TCA9548A discovery uses it; no TCA on this bench */
}

esp_err_t i2c_bus_write_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t data)
{
    (void)device;
    (void)mem_address;
    (void)data;
    return ESP_FAIL;
}

esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus, const i2c_device_config_t *config, i2c_master_dev_handle_t *device)
{
    assert(bus == &s_master_bus_token && config != NULL && device != NULL);
    for (size_t i = 0U; i < sizeof(s_master_devices) / sizeof(s_master_devices[0]); ++i) {
        if (!s_master_devices[i].active) {
            s_master_devices[i].address = (uint8_t)config->device_address;
            s_master_devices[i].active = true;
            *device = &s_master_devices[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    fake_master_device_t *fake = device;
    assert(fake != NULL && fake->active);
    fake->active = false;
    return ESP_OK;
}

static esp_err_t begin_transfer(fake_master_device_t *device, int timeout_ms)
{
    assert(device != NULL && device->active);
    ++s_transfers[task_index()];
    ++s_addr_transfers[device->address];
    int *fault = &s_timeout_faults[device->address];
    if (*fault != 0) {
        if (*fault > 0) --*fault;
        if (s_fault_code[device->address] == ESP_ERR_INVALID_STATE) {
            sensor_bus_lines_delay_us(TRANSFER_US);
            return ESP_ERR_INVALID_STATE;
        }
        sensor_bus_lines_delay_us((uint32_t)timeout_ms * 1000U);
        return ESP_ERR_TIMEOUT;
    }
    sensor_bus_lines_delay_us(TRANSFER_US);
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device, uint8_t *data, size_t data_len, int timeout_ms)
{
    fake_master_device_t *fake = device;
    const esp_err_t err = begin_transfer(fake, timeout_ms);
    if (err != ESP_OK) return err;
    memset(data, 0, data_len);
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device, const uint8_t *write_buffer, size_t write_size,
    uint8_t *read_buffer, size_t read_size, int timeout_ms)
{
    fake_master_device_t *fake = device;
    assert(write_size == 1U);
    const esp_err_t err = begin_transfer(fake, timeout_ms);
    if (err != ESP_OK) return err;
    const uint8_t reg = write_buffer[0];
    if (fake->address == INA_ADDR) {
        assert(read_size == 2U);
        const uint16_t value = ina_register(reg);
        read_buffer[0] = (uint8_t)(value >> 8);
        read_buffer[1] = (uint8_t)(value & 0xFFU);
    } else if (fake->address == BMP_ADDR) {
        bmp_read(reg, read_size, read_buffer);
    } else {
        memset(read_buffer, 0, read_size);
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t data_len, int timeout_ms)
{
    fake_master_device_t *fake = device;
    assert(data != NULL && data_len > 0U);
    const esp_err_t err = begin_transfer(fake, timeout_ms);
    if (err != ESP_OK) return err;
    if (fake->address == INA_ADDR) {
        assert(data_len == 3U);
        ina_write(data[0], (uint16_t)(((uint16_t)data[1] << 8) | data[2]));
    } else if (fake->address == BMP_ADDR) {
        assert(data_len == 2U);
        bmp_write(data[0], data[1]);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Simulation driver                                                          */
/* ------------------------------------------------------------------------- */

static nrf_topology_main_barometer_t s_topology;
static bool s_topology_enabled;
static int64_t s_topo_next_us;
static i2c_bus_device_handle_t s_remote;

static void ina_resume(void)
{
    assert(s_current == &s_main_token);
    assert(s_ina.created && s_ina.parked && !s_ina.finished);
    s_ina.parked = false;
    ++s_ina.resumes;
    s_current = &s_ina_token;
    baton_post(&s_ina.to_worker);
    baton_wait(&s_ina.to_main);
    s_current = &s_main_token;
    /* No task may stay owner of the bus after the worker yields, whatever happened to its transfers. */
    assert(s_lock.owner != &s_ina_token && s_lock.depth >= 0);
}

/* REMOTE refresh traffic: one owner session on the shared bus. */
static void remote_session(void)
{
    if (s_remote == NULL) return;
    if (sensor_bus_owner_acquire() != ESP_OK) return;
    uint8_t bytes[4] = {0};
    (void)sensor_bus_read_bytes(s_remote, 0x10U, sizeof(bytes), bytes);
    (void)sensor_bus_write_byte(s_remote, 0x11U, 1U);
    sensor_bus_owner_release();
}

/* One pass of the nRF topology task as far as MAIN is concerned. */
static void topology_cycle(void)
{
    assert(s_current == &s_main_token);
    nrf_topology_main_barometer_step(&s_topology, NOW_MS());
    remote_session();
    const uint32_t wait_ms = nrf_topology_main_barometer_wait_ms(&s_topology, NOW_MS(), 200U);
    s_topo_next_us = NOW_US() + (int64_t)(wait_ms > 0U ? wait_ms : 1U) * 1000;
}

static void sim_run_until(int64_t target_us)
{
    assert(s_current == &s_main_token);
    for (;;) {
        int64_t worker_due = INT64_MAX;
        if (s_ina.created && s_ina.parked && !s_ina.finished) {
            worker_due = s_ina.notifications > 0U ? NOW_US() : s_ina.wake_at_us;
        }
        const int64_t topo_due = s_topology_enabled ? s_topo_next_us : INT64_MAX;
        if (worker_due <= target_us && worker_due <= topo_due) {
            clock_advance_to(worker_due);
            ina_resume();
        } else if (topo_due <= target_us) {
            clock_advance_to(topo_due);
            topology_cycle();
        } else {
            clock_advance_to(target_us);
            return;
        }
    }
}

static void sim_run_ms(uint32_t duration_ms)
{
    sim_run_until(NOW_US() + (int64_t)duration_ms * 1000);
}

/* Used by ina3221_monitor_stop() while it waits for the worker to leave. */
void vTaskDelay(TickType_t ticks)
{
    assert(s_current == &s_main_token);
    sim_run_until(NOW_US() + (int64_t)ticks * INT64_C(1000000) / (int64_t)configTICK_RATE_HZ);
}

/* ------------------------------------------------------------------------- */
/* Observation helpers                                                        */
/* ------------------------------------------------------------------------- */

static ina3221_monitor_rail_t rail(size_t channel)
{
    ina3221_monitor_rail_t value;
    assert(ina3221_monitor_get_rail(channel, &value) == ESP_OK);
    return value;
}

static unsigned ina_measurements(void) { return s_ina_measurement_register_reads / 6U; }

static main_barometer_reading_t baro_reading(void)
{
    main_barometer_reading_t reading;
    main_barometer_get_reading(&s_topology.barometer, &reading);
    return reading;
}

/* Largest step between two successive accepted conversions published since `from`. */
static uint32_t max_sample_gap_ms(size_t from)
{
    uint32_t gap = 0U;
    uint32_t previous = 0U;
    for (size_t i = from; i < s_main_record_count; ++i) {
        const uint32_t sample = s_main_records[i].sample_ms;
        if (previous != 0U && sample > previous && sample - previous > gap) gap = sample - previous;
        if (sample != 0U) previous = sample;
    }
    return gap;
}

static unsigned distinct_samples_since(size_t from)
{
    unsigned count = 0U;
    uint32_t previous = 0U;
    for (size_t i = from; i < s_main_record_count; ++i) {
        const uint32_t sample = s_main_records[i].sample_ms;
        assert(sample >= previous); /* published conversion times never go backwards */
        if (sample > previous) ++count;
        previous = sample;
    }
    return count;
}

static void require_no_stray_warnings(size_t log_mark, const char *phase)
{
    (void)phase;
    assert(log_count_since(log_mark, "I2C_TIMEOUT_DIAG") == 0U);
    assert(log_count_since(log_mark, "I2C_BAROMETER_WARN") == 0U);
    assert(log_count_since(log_mark, "I2C_DEVICE_WARN") == 0U);
    assert(log_count_since(log_mark, "INA3221 read failed") == 0U);
}

/* ------------------------------------------------------------------------- */
/* Scenario                                                                   */
/* ------------------------------------------------------------------------- */

static void setup_world(void)
{
    s_logs = calloc(LOG_CAPACITY, LOG_LINE);
    assert(s_logs != NULL);
    fake_lines_reset();
    g_fake_lines.log_observer = log_capture;
    g_fake_lines.probe_configured = true;
    for (size_t address = 0U; address < 128U; ++address) {
        g_fake_lines.probe_result[address] = ESP_ERR_NOT_FOUND;
    }
    g_fake_lines.probe_result[INA_ADDR] = ESP_OK;
    g_fake_lines.probe_result[BMP_ADDR] = ESP_OK;
    g_fake_lines.probe_result[REMOTE_ADDR] = ESP_OK;
    assert(sensor_bus_lock_init() == ESP_OK);

    /* A healthy 1S pack under load plus both 5 V rails. */
    s_ina_bus_raw[0] = ina_bus_raw_from_volts(3.90f);
    s_ina_shunt_raw[0] = ina_shunt_raw_from_amps(0.25f);
    s_ina_bus_raw[1] = ina_bus_raw_from_volts(5.00f);
    s_ina_shunt_raw[1] = ina_shunt_raw_from_amps(-0.30f);
    s_ina_bus_raw[2] = ina_bus_raw_from_volts(5.00f);
    s_ina_shunt_raw[2] = ina_shunt_raw_from_amps(-0.05f);
}

/* 1. Normal operation: both devices on the real bus, MAIN calibrates. */
static void phase_normal_operation_and_calibration(void)
{
    nrf_topology_main_barometer_init(&s_topology);
    main_barometer_request_height_reference_capture(1U);
    assert(ina3221_monitor_start() == ESP_OK);
    s_topology_enabled = true;
    s_topo_next_us = NOW_US();
    {
        sensor_bus_handles_t handles = {0};
        assert(sensor_bus_get_handles(&handles) == ESP_OK);
        assert(sensor_bus_device_create(handles.bus, REMOTE_ADDR, SENSOR_BUS_I2C_SPEED_HZ, &s_remote) == ESP_OK);
    }

    const size_t log_mark = s_log_count;
    const size_t record_mark = s_main_record_count;
    sim_run_ms(7000U);

    /* INA3221: the worker really measured, about once a second. */
    assert(ina_measurements() >= 5U);
    for (size_t channel = 0U; channel < 3U; ++channel) {
        const ina3221_monitor_rail_t r = rail(channel);
        assert(r.available && r.valid && r.age_ms <= 1200U);
    }
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).bus_voltage_v > 3.8f && rail(INA3221_MONITOR_RAIL_BATTERY).bus_voltage_v < 4.0f);
    assert(rail(INA3221_MONITOR_RAIL_ESP32_5V).bus_voltage_v > 4.9f);
    assert(s_power_last_valid && s_power_valid_calls >= 5U);
    assert(s_transfers[1] > 0U && s_transfers[0] > 0U);

    /* MAIN: the production state machine converted, calibrated, and published. */
    const main_barometer_reading_t reading = baro_reading();
    assert(reading.bmp280_present && reading.bmp280_address == BMP_ADDR);
    assert(reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(reading.height_reference_generation == 1U);
    assert(reading.bmp280_bus_error_count == 0U && reading.config_lost_count == 0U);
    assert(reading.bmp280_probe_count == 1U);
    assert(reading.pressure_pa >= 100600U && reading.pressure_pa <= 100700U);
    assert(distinct_samples_since(record_mark) >= 8U);
    /* Each published sample is a distinct conversion of the sensor: none was read twice. */
    assert(s_bmp.duplicate_reads == 0U);
    assert(s_bmp.forced_writes == 0U);
    assert(max_sample_gap_ms(record_mark) <= 400U);
    const main_record_t *last = &s_main_records[s_main_record_count - 1U];
    assert(last->reference == DEVICE_MAIN_HEIGHT_REFERENCE_VALID && last->generation == 1U &&
           last->relative_z_mm > -100 && last->relative_z_mm < 100);   /* sensor noise around the reference */
    require_no_stray_warnings(log_mark, "normal");
    printf("E2E normal: ina_measurements=%u bmp_data_reads=%u distinct_samples=%u max_gap_ms=%u pressure=%u Pa\n",
           ina_measurements(), s_bmp.data_reads, distinct_samples_since(record_mark),
           (unsigned)max_sample_gap_ms(record_mark), (unsigned)reading.pressure_pa);
}

/* 2. One BMP280 transfer times out: MAIN recovers fast and INA3221 does not notice. */
static void phase_bmp_transient_fault(void)
{
    const size_t log_mark = s_log_count;
    const size_t record_mark = s_main_record_count;
    const main_barometer_reading_t before = baro_reading();
    const unsigned ina_before = ina_measurements();
    const int64_t fault_at_us = NOW_US();

    s_timeout_faults[BMP_ADDR] = 1;
    sim_run_ms(2500U);

    const main_barometer_reading_t after = baro_reading();
    assert(s_timeout_faults[BMP_ADDR] == 0);                       /* the fault really hit a BMP280 transfer */
    assert(after.bmp280_bus_error_count == before.bmp280_bus_error_count + 1U);
    assert(after.bmp280_probe_count == before.bmp280_probe_count); /* no full re-probe for one timeout */
    assert(after.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    /* Fast recovery: new conversions keep arriving well inside the 5 s backoff. */
    assert(distinct_samples_since(record_mark) >= 5U);
    assert(max_sample_gap_ms(record_mark) <= 1000U);
    assert(s_main_records[s_main_record_count - 1U].sample_ms * INT64_C(1000) > fault_at_us);
    /* INA3221 shares the bus and never saw a fault. */
    assert(ina_measurements() >= ina_before + 2U);
    assert(log_count_since(log_mark, "INA3221 read failed") == 0U);
    assert(log_count_since(log_mark, "INA3221_BUS_BUSY_SKIPPED") == 0U);
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).valid);
    assert(s_bmp.duplicate_reads == 0U);
}

/* 3. One INA3221 transfer times out: the monitor backs off, MAIN is untouched, the bus is free. */
static void phase_ina_transient_fault(void)
{
    const size_t log_mark = s_log_count;
    const size_t record_mark = s_main_record_count;
    const main_barometer_reading_t before = baro_reading();
    const unsigned ina_before = ina_measurements();
    const int64_t fault_at_us = NOW_US();

    s_timeout_faults[INA_ADDR] = 1;
    sim_run_ms(9000U);

    const main_barometer_reading_t after = baro_reading();
    assert(s_timeout_faults[INA_ADDR] == 0);
    assert(log_count_since(log_mark, "INA3221 read failed") == 1U);
    /* BMP280 neither failed nor lost a sample because INA3221 did. */
    assert(after.bmp280_bus_error_count == before.bmp280_bus_error_count);
    assert(after.bmp280_probe_count == before.bmp280_probe_count);
    assert(after.config_lost_count == before.config_lost_count);
    assert(max_sample_gap_ms(record_mark) <= 500U);
    assert(distinct_samples_since(record_mark) >= 30U);
    /* INA3221 came back with fresh data after its own bounded backoff. */
    const ina3221_monitor_rail_t battery = rail(INA3221_MONITOR_RAIL_BATTERY);
    assert(battery.valid && battery.age_ms <= 1200U && battery.sampled_at_us > fault_at_us);
    assert(ina_measurements() > ina_before);
    /* The failed transfer left no owner behind: any task can take the bus now. */
    assert(s_lock.owner == NULL);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    assert(log_count_since(log_mark, "I2C_BAROMETER_WARN") == 0U);
}

/* 3b. The same single failure as the IDF 5.5 controller reports it when the bus was busy right after another task
 * (2026-11-05, 15.7 s after boot, core 0 saturated): ESP_ERR_INVALID_STATE while the address still ACKs. Both devices
 * must come back within a poll or two, not after the 5 s re-probe / re-open backoff that only a really absent device
 * deserves. */
static void phase_transient_invalid_state_recovers_quickly(void)
{
    {   /* BMP280 */
        const size_t record_mark = s_main_record_count;
        const main_barometer_reading_t before = baro_reading();
        s_fault_code[BMP_ADDR] = ESP_ERR_INVALID_STATE;
        s_timeout_faults[BMP_ADDR] = 1;
        sim_run_ms(4000U);
        s_fault_code[BMP_ADDR] = 0;
        const main_barometer_reading_t after = baro_reading();
        assert(s_timeout_faults[BMP_ADDR] == 0);
        assert(after.bmp280_bus_error_count == before.bmp280_bus_error_count + 1U);
        assert(after.bmp280_probe_count == before.bmp280_probe_count);       /* no re-probe for one failed transfer */
        assert(max_sample_gap_ms(record_mark) <= 1000U);                     /* not the 5 s backoff */
        assert(distinct_samples_since(record_mark) >= 10U);                  /* 4 s of ~0.2 s polls minus the failed one */
        assert(NOW_MS() - after.last_sample_ms <= 1000U);                    /* and it is still converting at the end */
    }
    {   /* INA3221: its re-open backoff after a failed read is the MAX-440 contract (5 s, test I3), so the power data
         * is blind for about 5 s here; the point is that it never goes invalid (stale limit 10 s) and comes back. */
        const size_t log_mark = s_log_count;
        const int64_t fault_at_us = NOW_US();
        s_fault_code[INA_ADDR] = ESP_ERR_INVALID_STATE;
        s_timeout_faults[INA_ADDR] = 1;
        const unsigned invalid_before = s_power_invalid_calls;
        sim_run_ms(7000U);
        s_fault_code[INA_ADDR] = 0;
        assert(s_timeout_faults[INA_ADDR] == 0);
        assert(log_count_since(log_mark, "INA3221 read failed") == 1U);
        assert(s_power_invalid_calls == invalid_before);                     /* never published as invalid */
        const ina3221_monitor_rail_t battery = rail(INA3221_MONITOR_RAIL_BATTERY);
        assert(battery.valid && battery.age_ms <= 1200U && battery.sampled_at_us > fault_at_us);
        assert(s_lock.owner == NULL);
    }
}

/* 4. The topology task holds a long bus session: INA3221 reports contention, not a dead sensor. */
static void phase_owner_contention(void)
{
    const size_t log_mark = s_log_count;
    const unsigned ina_before = ina_measurements();
    const unsigned device_errors_before = s_power_invalid_calls;

    assert(sensor_bus_owner_acquire() == ESP_OK);
    s_topology_enabled = false;
    sim_run_ms(2500U);
    sensor_bus_owner_release();

    /* The held bus stopped INA3221 reads; the cause is named and rate limited. */
    assert(ina_measurements() == ina_before);
    assert(log_count_since(log_mark, "INA3221_BUS_BUSY_SKIPPED reason=owner_busy") >= 1U);
    assert(log_count_since(log_mark, "INA3221_BUS_BUSY_SKIPPED reason=owner_busy") <= 2U);
    assert(log_count_since(log_mark, "INA3221 read failed") == 0U);
    assert(log_count_since(log_mark, "INA3221_PAUSED") == 0U);
    assert(s_power_invalid_calls == device_errors_before);
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).valid);

    s_topology_enabled = true;
    s_topo_next_us = NOW_US();
    sim_run_ms(2500U);
    assert(ina_measurements() > ina_before);
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).valid && rail(INA3221_MONITOR_RAIL_BATTERY).age_ms <= 1200U);

    /* The per-address bus statistics must not turn the skipped reads into INA3221 device failures
     * (2026-11-05 hardware log: "addr=0x40 ... other=3 last_err=ERROR" after three owner-busy skips). */
    sim_run_ms(12000U);
    for (size_t i = log_mark; i < s_log_count; ++i) {
        if (strstr(s_logs[i], "I2C_BUS_SUMMARY addr=0x40") != NULL) assert(strstr(s_logs[i], " other=0 ") != NULL);
    }
}

/* 5. BLE ranging: the topology task takes the quiet lease for 20 s. */
static void phase_quiet_lease(void)
{
    const size_t log_mark = s_log_count;
    const main_barometer_reading_t before = baro_reading();
    const unsigned ina_transfers_before = s_transfers[1];
    const unsigned ina_measurements_before = ina_measurements();
    const unsigned total_before = s_transfers[0] + s_transfers[1];
    const unsigned valid_publishes_before = s_power_valid_calls;

    assert(sensor_bus_quiet_begin(30000U) == ESP_OK);
    const int64_t lease_start_us = NOW_US();
    const unsigned resumes_at_begin = s_ina.resumes;
    s_topology_enabled = false;                      /* the topology task is busy ranging */

    sim_run_ms(5000U);
    assert(s_transfers[1] == ina_transfers_before);  /* not one INA3221 transaction in the lease */
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).valid);
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).age_ms >= 4000U);   /* ... and the age keeps growing */

    ina3221_monitor_request_fast_sampling(4000U);    /* even a 20 ms request must not touch the bus */
    sim_run_ms(5000U);
    assert(s_transfers[1] == ina_transfers_before);

    /* The lease owner keeps its access: one MAIN poll goes through while INA3221 stays silent. */
    const unsigned owner_transfers_before = s_transfers[0];
    topology_cycle();
    assert(s_transfers[0] > owner_transfers_before);
    assert(s_transfers[1] == ina_transfers_before);

    sim_run_ms(3000U);                               /* 13 s into the lease: stale limit (10 s) has passed */
    {
        const ina3221_monitor_rail_t battery = rail(INA3221_MONITOR_RAIL_BATTERY);
        assert(!battery.valid && battery.stale && battery.age_ms >= 10000U);
        assert(!s_power_last_valid);                 /* old values are never passed off as fresh */
    }
    sim_run_until(lease_start_us + INT64_C(20000000));

    assert(s_transfers[1] == ina_transfers_before);
    assert(ina_measurements() == ina_measurements_before);
    assert(s_power_valid_calls == valid_publishes_before);
    assert(log_count_since(log_mark, "INA3221_PAUSED reason=i2c_quiet") == 1U);
    assert(log_count_since(log_mark, "I2C_TIMEOUT_DIAG") == 0U);       /* no per-second bus warnings */
    assert(log_count_since(log_mark, "INA3221 read failed") == 0U);
    assert(log_count_since(log_mark, "I2C_DEVICE_WARN") == 0U);
    assert(log_count_since(log_mark, "INA3221_BUS_BUSY_SKIPPED") == 0U);   /* intended silence is not contention */
    const unsigned resumes_in_lease = s_ina.resumes - resumes_at_begin;
    assert(resumes_in_lease >= 50U);                 /* includes the 20 ms fast-sampling wake-ups */

    sensor_bus_quiet_end();
    const int64_t lease_end_us = NOW_US();
    {
        const char *end_line = log_find_since(log_mark, "I2C_BUS_QUIET end");
        assert(end_line != NULL);
        /* The worker never asked the bus for a transfer. The only refusals the bus counted are RAM-only calls
         * (the periodic sensor_bus_log_diag_summary() reads the recovery status under the bus lock): a polling
         * worker would have produced one refusal per wake-up, i.e. dozens. */
        const char *skipped = strstr(end_line, "skipped=");
        assert(skipped != NULL && atoi(skipped + 8) <= 2);
    }
    sim_run_ms(1500U);
    s_topology_enabled = true;
    s_topo_next_us = NOW_US();
    sim_run_ms(3000U);

    /* INA3221 resumes by itself, reports exactly what it skipped, and publishes fresh data. */
    assert(log_count_since(log_mark, "INA3221_RESUMED reason=i2c_quiet") == 1U);
    {
        const char *resumed = log_find_since(log_mark, "INA3221_RESUMED reason=i2c_quiet");
        char expected[48];
        (void)snprintf(expected, sizeof(expected), "skipped=%u", resumes_in_lease);
        assert(strstr(resumed, expected) != NULL);
    }
    assert(ina_measurements() > ina_measurements_before);
    assert(s_power_last_valid && s_power_last_valid_at_us > lease_end_us);
    {
        const ina3221_monitor_rail_t battery = rail(INA3221_MONITOR_RAIL_BATTERY);
        assert(battery.valid && battery.age_ms <= 1200U && battery.sampled_at_us > lease_end_us);
    }
    assert(log_count_since(log_mark, "INA3221 read failed") == 0U);

    /* MAIN was not declared broken because the bus was silent on purpose. */
    const main_barometer_reading_t after = baro_reading();
    assert(after.bmp280_bus_error_count == before.bmp280_bus_error_count);
    assert(after.bmp280_probe_count == before.bmp280_probe_count);
    assert(after.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(after.last_sample_ms * INT64_C(1000) > lease_end_us);
    assert(s_transfers[0] + s_transfers[1] > total_before);
    assert(s_bmp.duplicate_reads == 0U);
    printf("E2E quiet lease: ina_resumes_in_lease=%u ina_transfers_in_lease=0 resumed_log_matches\n", resumes_in_lease);
}

/* 6. Re-calibration: a new generation starts from new conversions, never from old samples. */
static void phase_recalibration(void)
{
    const size_t record_mark = s_main_record_count;
    const int64_t request_us = NOW_US();

    assert(main_barometer_request_height_reference_capture(2U) == ESP_OK);
    sim_run_ms(5000U);

    const main_barometer_reading_t reading = baro_reading();
    assert(reading.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
    assert(reading.height_reference_generation == 2U);
    bool saw_not_established = false;
    for (size_t i = record_mark; i < s_main_record_count; ++i) {
        const main_record_t *record = &s_main_records[i];
        if (record->generation == 2U && record->reference == DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED) {
            saw_not_established = true;
        }
        if (record->generation == 2U && record->reference == DEVICE_MAIN_HEIGHT_REFERENCE_VALID) {
            assert(saw_not_established);                                  /* never valid before the new capture ran */
            assert((int64_t)record->sample_ms * 1000 > request_us);       /* from conversions made after the request */
        }
    }
    assert(saw_not_established);
    assert(s_main_records[s_main_record_count - 1U].generation == 2U);
    assert(s_main_records[s_main_record_count - 1U].relative_z_mm > -100 &&
           s_main_records[s_main_record_count - 1U].relative_z_mm < 100);
    assert(rail(INA3221_MONITOR_RAIL_BATTERY).valid);
    assert(s_bmp.duplicate_reads == 0U);
}

/* 7. Thirty minutes of the operator's routine: calibrate, wait, range (quiet lease), with a bus fault of each kind in
 * every round. Everything the earlier phases proved has to keep holding, and nothing may grow with the run time. */
static void phase_thirty_minutes(void)
{
    const main_barometer_reading_t before = baro_reading();
    const unsigned data_reads_before = s_bmp.data_reads;
    unsigned lines_per_round[18];
    uint16_t generation = 100U;
    for (unsigned round = 0U; round < 18U; ++round) {          /* 18 x 100 s */
        const size_t round_mark = s_log_count;
        sim_run_ms(30000U);                                        /* idle in the measurement menu */
        s_timeout_faults[BMP_ADDR] = 1;
        sim_run_ms(10000U);
        assert(s_timeout_faults[BMP_ADDR] == 0);
        s_timeout_faults[INA_ADDR] = 1;
        sim_run_ms(15000U);
        assert(s_timeout_faults[INA_ADDR] == 0);
        assert(main_barometer_request_height_reference_capture(++generation) == ESP_OK);   /* calibration */
        sim_run_ms(10000U);
        assert(sensor_bus_quiet_begin(30000U) == ESP_OK);          /* ranging */
        s_topology_enabled = false;
        sim_run_ms(15000U);
        sensor_bus_quiet_end();
        s_topology_enabled = true;
        s_topo_next_us = NOW_US();
        sim_run_ms(20000U);

        const ina3221_monitor_rail_t battery = rail(INA3221_MONITOR_RAIL_BATTERY);
        assert(battery.valid && battery.age_ms <= 1200U);
        assert(s_lock.owner == NULL);
        const main_barometer_reading_t now = baro_reading();
        assert(now.height_reference_state == MAIN_BAROMETER_REFERENCE_VALID);
        assert(now.height_reference_generation == generation);
        assert(NOW_MS() - now.last_sample_ms <= 1000U);
        assert(log_count_since(round_mark, "INA3221_PAUSED reason=i2c_quiet") == 1U);
        assert(log_count_since(round_mark, "INA3221_RESUMED reason=i2c_quiet") == 1U);
        assert(log_count_since(round_mark, "I2C_TIMEOUT_DIAG src=bus_owner") == 0U);
        lines_per_round[round] = (unsigned)(s_log_count - round_mark);
    }
    const main_barometer_reading_t after = baro_reading();
    assert(after.bmp280_bus_error_count == before.bmp280_bus_error_count + 18U);   /* exactly the injected timeouts */
    assert(after.bmp280_probe_count == before.bmp280_probe_count);                 /* never re-probed */
    assert(after.config_lost_count == before.config_lost_count);
    assert(s_bmp.duplicate_reads == 0U && s_bmp.forced_writes == 0U);
    assert(s_bmp.data_reads - data_reads_before > 4000U);          /* it really kept converting for 30 minutes */
    assert(s_log_count < LOG_CAPACITY / 2U);                       /* the log did not run away */
    /* Nothing grows with the run time: the last round is as quiet as the first. */
    assert(lines_per_round[17] <= lines_per_round[0] + lines_per_round[0] / 2U + 10U);
    printf("E2E 30 min: rounds=18 log_lines_first=%u last=%u bmp_data_reads=%u bus_errors=%u\n",
           lines_per_round[0], lines_per_round[17], s_bmp.data_reads - data_reads_before,
           (unsigned)after.bmp280_bus_error_count);
}

/* 8. Battery pack, BLE ranging: the quiet lease blinds the software power monitor. The cell sags below the critical
 * level during the lease; nothing can react until the monitor reads again, then the existing confirm + grace runs.
 * This is the documented limit (the owner allowed quiet with stale power data): the test pins its size. */
static unsigned s_critical_calls;
static int64_t s_critical_at_us;
static void critical_battery_handler(void)
{
    ++s_critical_calls;
    s_critical_at_us = NOW_US();
}

static void phase_battery_quiet_blind_window(void)
{
    const unsigned alerts_before = s_power_alerts;
    ina3221_monitor_set_critical_battery_handler(critical_battery_handler);
    sim_run_ms(20000U);
    assert(s_critical_calls == 0U && s_power_alerts == alerts_before);     /* healthy pack: nothing */

    assert(sensor_bus_quiet_begin(120000U) == ESP_OK);
    s_topology_enabled = false;
    sim_run_ms(30000U);
    s_ina_bus_raw[0] = ina_bus_raw_from_volts(3.20f);                  /* the cell sags below 3.30 V mid-lease */
    sim_run_ms(60000U);
    assert(s_critical_calls == 0U);                                    /* blind: nothing can have been noticed */
    assert(s_power_alerts == alerts_before);
    sensor_bus_quiet_end();
    const int64_t lease_end_us = NOW_US();
    s_topology_enabled = true;
    s_topo_next_us = NOW_US();
    sim_run_ms(30000U);

    /* First reading <= 1 s after the lease, 5 samples to confirm, then the 10 s grace: ~15 s, never longer than 20. */
    assert(s_critical_calls == 1U);
    assert(s_critical_at_us > lease_end_us + INT64_C(10000000));
    assert(s_critical_at_us <= lease_end_us + INT64_C(20000000));
    assert(s_power_alerts == alerts_before + 2U);                      /* CRITICAL_GRACE, then SHUTDOWN */
    printf("E2E battery quiet: blind %d s, shutdown requested %d ms after the lease ended\n",
           90, (int)((s_critical_at_us - lease_end_us) / 1000));
}

/* 9. Shutdown: the worker leaves cleanly, no deadlock, the bus is released. */
static void phase_shutdown(void)
{
    s_topology_enabled = false;
    ina3221_monitor_stop();
    assert(s_ina.finished);
    thread_join(s_ina.thread);
    assert(s_lock.owner == NULL && s_lock.depth == 0);
    nrf_topology_main_barometer_close(&s_topology);
    assert(sensor_bus_device_delete(&s_remote) == ESP_OK);
    assert(sensor_bus_deinit() == ESP_OK);
    assert(s_power_alerts == 2U);                    /* only the two alerts of the battery phase */
}

int main(void)
{
    setup_world();
    phase_normal_operation_and_calibration();
    phase_bmp_transient_fault();
    phase_ina_transient_fault();
    phase_transient_invalid_state_recovers_quickly();
    phase_owner_contention();
    phase_quiet_lease();
    phase_recalibration();
    phase_thirty_minutes();
    phase_battery_quiet_blind_window();
    phase_shutdown();
    free(s_logs);
    printf("E2E baro+INA3221+bus: PASS (bmp_data_reads=%u duplicate_reads=%u ina_measurements=%u)\n",
           s_bmp.data_reads, s_bmp.duplicate_reads, ina_measurements());
    return 0;
}
