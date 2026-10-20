#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "nrf_bootstrap.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_log_protocol.h"
#include "nrf_node_state.h"
#include "nrf_topology_service.h"
#include "nrf_topology_periods.h"
#include "nrf_upstream_protocol.h"
#include "sensor_bus.h"
#include "sensor_bus_lines.h"
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
#include "device_status.h"
#endif

/*
 * MAX-246 item 5: nrf_topology_service integration on a simulated wire.
 *
 * Real ESP32 side: sensor_bus (TCA discovery + NACK classification),
 * tca9548a_mux, nrf_topology, nrf_sensor_transport, the service worker and
 * CS authority. Real nRF side: every XIAO is an nrf_bootstrap state
 * machine answering the upstream protocol. Only the wire is simulated,
 * with ESP-IDF 5.5 semantics: a NACK is ESP_ERR_INVALID_STATE and an
 * address probe finds nobody (MAX-248).
 *
 * The TCA9548A model is the real chip: any combination of channel bits
 * can be enabled. The wire asserts on every transfer that no two targets
 * answer at the same address -- the one-hot invariant the firmware owes
 * the shared REMOTE address 0x32.
 *
 * The worker runs inline in the task-create stub. Its ulTaskNotifyTake()
 * consumes a script (advance time / run an action / stop); STOP calls the
 * real nrf_topology_service_stop(), whose wait loop yields back into the
 * worker.
 */

#define TCA_ADDRESS 0x70U
#define INA_ADDRESS 0x40U
#define MAX_NODES 10
#define UPSTREAM_CHANNEL 0xFFU

/* ------------------------------------------------------------------ wire */

typedef struct {
    bool present;
    uint8_t channel;              /* UPSTREAM_CHANNEL or TCA channel 0..7 */
    uint8_t address;
    uint8_t hardware_id[4];
    nrf_node_state_t node_state;
    nrf_bootstrap_t bootstrap;
    uint32_t sensor_request_id;
    uint8_t snapshot_version;
    uint8_t snapshot_result;
    int32_t relative_z_mm;
    uint8_t reference_state;
    uint16_t reference_generation;
    int16_t gyro_bias_mdps[3];
    int16_t gravity_ref_mg[3];
    uint8_t imu_cal_state;
    uint16_t imu_cal_generation;
    bool delayed_assign_pending;
    nrf_upstream_assign_t delayed_assign;
    unsigned height_reference_captures;
    uint16_t captured_generation[8];
    unsigned imu_calibration_requests;
    uint16_t imu_calibration_generations[8];
    nrf_cs_control_request_t cs_request;
    nrf_cs_control_request_t cs_status_request;
    bool cs_request_valid;
    bool cs_status_request_valid;
    unsigned status_reads;
    unsigned sensor_refreshes;
    unsigned cs_execute_writes;
    unsigned cs_status_reads;
} sim_node_t;

typedef struct {
    uint8_t address;
    bool live;
} sim_device_t;

static sim_node_t s_nodes[MAX_NODES];
static bool s_tca_present;
static uint8_t s_tca_control;
static unsigned s_tca_writes;
static bool s_allow_address_collisions;
static sim_device_t s_devices[64];
static int s_bus_token;
static int s_master_bus_token;
static bool s_bus_live;
static uint32_t s_tick;
static uint64_t s_fake_subtick_us;
static uint32_t s_fake_transaction_duration_us;
static unsigned s_fail_recursive_lock_attempts;
static uint32_t s_last_notify_wait_ms;
static uint32_t s_max_bus_owner_hold_ms;
static unsigned s_bus_owner_log_count;
static unsigned s_collisions;
static unsigned s_direct_nrf_read_calls;
static unsigned s_direct_nrf_write_calls;
static unsigned s_direct_nrf_probe_calls;
static unsigned s_controller_reset_calls;
static bool s_fail_i2c_bus_create;
static bool s_fail_mutex_create;
static bool s_fail_task_create;
static bool s_fail_next_status_read;
static unsigned s_timeout_failures_until_stuck;
static unsigned s_nack_read_calls;
static unsigned s_nack_byte_read_calls;
static unsigned s_probe_calls;
static bool s_scl_stuck;
static bool s_stop_on_status_read;
static bool s_reconcile_stop_waiting;
enum { STATUS_RESPONSE_FRAME_CAPACITY = 2U };
/* Log responses can be 136 bytes; using the upstream STATUS size here truncated their storage. */
static uint8_t s_status_response_frames[STATUS_RESPONSE_FRAME_CAPACITY][NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
static size_t s_status_response_frame_count;
static size_t s_status_response_frame_index;
static bool s_arm_reselection_probe_after_status_read;
static bool s_reselection_probe_armed;
static bool s_fail_reselection_probe;
static jmp_buf s_reconcile_stop_return;
static unsigned s_malformed_cs_results_remaining;
static unsigned s_malformed_cs_results_seen;
static unsigned s_malformed_sensor_snapshots_remaining;
static unsigned s_malformed_sensor_snapshots_seen;
static bool s_delay_next_assignment_confirmation;
static unsigned s_delayed_assignment_confirmations;
static unsigned s_cs_execute_count;
static unsigned s_task_create_count;
static unsigned s_task_delete_count;
static unsigned s_active_tasks;
static UBaseType_t s_stack_hwm_default = 2048U;
static UBaseType_t s_stack_hwm_sequence[256];
static size_t s_stack_hwm_sequence_count;
static size_t s_stack_hwm_sequence_pos;
static unsigned s_stack_hwm_warning_logs;
static char s_long_error_name[512];
static bool s_long_error_was_logged;
static unsigned s_tca_fault_set_logs;
static unsigned s_tca_bus_stuck_logs;
static bool s_in_height_calibration_handler;
static bool s_attempt_height_request_during_capture;
static bool s_height_request_during_capture_attempted;
static esp_err_t s_height_request_during_capture_result;
static uint16_t s_height_request_during_capture_generation;
static unsigned s_height_reference_writes;
static nrf_upstream_height_reference_t s_height_reference_requests[32];
static bool s_in_imu_calibration_handler;
static unsigned s_imu_calibration_writes;
static nrf_upstream_imu_calibrate_t s_imu_calibration_requests[32];
static bool s_cs_hold_pending;
static unsigned s_status_reads_before_backoff;
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
static bool s_cs_use_pair_distances;
static int32_t s_cs_pair_distances[NRF_CS_MAX_PAIRS];
static int32_t s_cs_fail_pair_index = -1;
static device_status_snapshot_t s_device_status;
static nrf_topology_ranging_progress_t s_seen_ranging_progress;
static nrf_topology_ranging_result_t s_seen_frozen_ranging_result;
static int32_t s_last_uncertainty_mm[NRF_CS_MAX_NODES];
static size_t s_ranging_capture_index;

esp_err_t device_status_get_snapshot(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_device_status;
    return ESP_OK;
}
#endif

static void reset_world(void);
static void run_service(void);

const char *test_esp_err_to_name(int error)
{
    return error == ESP_ERR_TIMEOUT && s_long_error_name[0] != '\0'
        ? s_long_error_name
        : "fake_error";
}

void test_log_record(char level, const char *tag, const char *format, ...)
{
    char line[1024];
    va_list args;
    (void)tag;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (level == 'W' && strstr(line, "STACK_HWM") != NULL) {
        ++s_stack_hwm_warning_logs;
    }
    if (s_long_error_name[0] != '\0' && strstr(line, s_long_error_name) != NULL) {
        s_long_error_was_logged = true;
    }
    if (strstr(line, "TCA_FAULT_SET") != NULL) ++s_tca_fault_set_logs;
    if (strstr(line, "TCA_FAULT_SET reason=TCA_BUS_STUCK") != NULL) ++s_tca_bus_stuck_logs;
    if (strstr(line, "I2C_BUS_OWNER owner=") != NULL) {
        const char *hold = strstr(line, "hold_ms=");
        ++s_bus_owner_log_count;
        if (hold != NULL) {
            const unsigned long long hold_ms = strtoull(hold + strlen("hold_ms="), NULL, 10);
            if (hold_ms > s_max_bus_owner_hold_ms) {
                s_max_bus_owner_hold_ms = (uint32_t)hold_ms;
            }
        }
    }
}

static int node_readdress(void *context, uint8_t address)
{
    ((sim_node_t *)context)->address = address;
    return 0;
}

static bool node_registered(void *context)
{
    return ((sim_node_t *)context)->present;
}

static uint8_t node_address(void *context)
{
    return ((sim_node_t *)context)->address;
}

static void node_cold_reboot(void *context)
{
    (void)context;
}

static void node_boot(sim_node_t *node, uint8_t channel, uint8_t id)
{
    memset(node, 0, sizeof(*node));
    node->present = true;
    node->snapshot_version = 3U;
    node->snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_NO_DATA;
    node->channel = channel;
    node->address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
    node->hardware_id[0] = 0xA0U;
    node->hardware_id[1] = 0x55U;
    node->hardware_id[2] = id;
    node->hardware_id[3] = (uint8_t)(id ^ 0x5AU);
    const nrf_bootstrap_ops_t ops = {
        .readdress = node_readdress,
        .target_is_registered = node_registered,
        .target_address = node_address,
        .cold_reboot = node_cold_reboot,
        .context = node,
    };
    nrf_node_state_init(&node->node_state);
    assert(nrf_node_state_mark_ready(&node->node_state) == NRF_NODE_STATE_OK);
    assert(nrf_bootstrap_init(&node->bootstrap, &node->node_state,
                              node->hardware_id, sizeof(node->hardware_id), &ops) == NRF_UPSTREAM_RESULT_OK);
}

static bool node_visible(const sim_node_t *node)
{
    if (!node->present) {
        return false;
    }
    if (node->channel == UPSTREAM_CHANNEL) {
        return true;
    }
    return s_tca_present && (s_tca_control & (uint8_t)(1U << node->channel)) != 0U;
}

/* The single target answering at `address`, or NULL (NACK). */
static sim_node_t *node_at(uint8_t address)
{
    sim_node_t *found = NULL;
    for (int i = 0; i < MAX_NODES; ++i) {
        if (node_visible(&s_nodes[i]) && s_nodes[i].address == address) {
            if (found != NULL) {
                ++s_collisions;
                assert(s_allow_address_collisions);
                continue;
            }
            found = &s_nodes[i];
        }
    }
    /* Two XIAOs driving one address corrupt every frame on real wires. */
    assert(s_collisions == 0U || s_allow_address_collisions);
    return found;
}

static bool address_acks(uint8_t address)
{
    if (address == INA_ADDRESS) {
        return true;
    }
    if (address == TCA_ADDRESS) {
        return s_tca_present;
    }
    return node_at(address) != NULL;
}

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    (void)port;
    (void)config;
    if (s_fail_i2c_bus_create) {
        return NULL;
    }
    if (s_scl_stuck) {
        s_tick += pdMS_TO_TICKS(120000U); /* clear the bounded recovery backoff from earlier lifecycle cycles */
    }
    assert(!s_bus_live);
    s_bus_live = true;
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
    s_bus_live = false;
    *bus = NULL;
    return ESP_OK;
}

i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t speed_hz)
{
    assert(bus == &s_bus_token);
    assert(speed_hz == SENSOR_BUS_I2C_SPEED_HZ);
    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
        if (!s_devices[i].live) {
            s_devices[i].live = true;
            s_devices[i].address = address;
            return &s_devices[i];
        }
    }
    assert(false);
    return NULL;
}

esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device)
{
    assert(bus == &s_master_bus_token);
    assert(config != NULL && device != NULL);
    *device = i2c_bus_device_create(
        &s_bus_token, (uint8_t)config->device_address, config->scl_speed_hz);
    return *device != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    i2c_bus_device_handle_t managed_device = device;
    return i2c_bus_device_delete(&managed_device);
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    assert(device != NULL && *device != NULL);
    sim_device_t *d = *device;
    assert(d->live);
    d->live = false;
    *device = NULL;
    return ESP_OK;
}

static sim_device_t *device_of(i2c_bus_device_handle_t handle)
{
    sim_device_t *d = handle;
    assert(d != NULL && d->live);
    return d;
}

static void fake_i2c_transaction_elapsed(void)
{
    s_fake_subtick_us += s_fake_transaction_duration_us;
}

esp_err_t i2c_master_receive(
    i2c_master_dev_handle_t device, uint8_t *data, size_t data_len, int timeout_ms)
{
    (void)timeout_ms;
    sim_device_t *target = device_of((i2c_bus_device_handle_t)device);
    if (s_scl_stuck && target->address == TCA_ADDRESS) {
        fake_i2c_transaction_elapsed();
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (target->address == TCA_ADDRESS && data_len == 1U) {
        err = i2c_bus_read_byte(
            (i2c_bus_device_handle_t)device, NULL_I2C_MEM_ADDR, data);
    } else {
        err = i2c_bus_read_bytes(
            (i2c_bus_device_handle_t)device, NULL_I2C_MEM_ADDR, data_len, data);
    }
    fake_i2c_transaction_elapsed();
    return err;
}

esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms)
{
    (void)timeout_ms;
    assert(write_buffer != NULL && write_size == 1U);
    if (s_scl_stuck && device_of((i2c_bus_device_handle_t)device)->address == TCA_ADDRESS) {
        fake_i2c_transaction_elapsed();
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = i2c_bus_read_bytes(
        (i2c_bus_device_handle_t)device, write_buffer[0], read_size, read_buffer);
    fake_i2c_transaction_elapsed();
    return err;
}

esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device,
    const uint8_t *data,
    size_t data_len,
    int timeout_ms)
{
    sim_device_t *target = device_of((i2c_bus_device_handle_t)device);
    (void)timeout_ms;
    assert(data != NULL && data_len > 0U);
    if (s_scl_stuck && target->address == TCA_ADDRESS) {
        fake_i2c_transaction_elapsed();
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (target->address == TCA_ADDRESS && data_len == 1U) {
        err = i2c_bus_write_byte(
            (i2c_bus_device_handle_t)device, NULL_I2C_MEM_ADDR, data[0]);
    } else {
        err = i2c_bus_write_bytes(
            (i2c_bus_device_handle_t)device, NULL_I2C_MEM_ADDR, data_len, data);
    }
    fake_i2c_transaction_elapsed();
    return err;
}

esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    if (d->address == TCA_ADDRESS && s_tca_present) {
        *data = s_tca_control;
        return ESP_OK;
    }
    if (d->address >= 0x70U && d->address <= 0x77U) {
        ++s_nack_byte_read_calls;
        return ESP_ERR_INVALID_STATE; /* IDF 5.5 NACK */
    }
    assert(false); /* no other single-byte reads on this bus */
    return ESP_FAIL;
}

esp_err_t i2c_bus_write_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    assert(d->address == TCA_ADDRESS);
    if (!s_tca_present) {
        return ESP_ERR_INVALID_STATE;
    }
    s_tca_control = data;
    ++s_tca_writes;
    return ESP_OK;
}

esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    if (s_tca_control == 0U && d->address >= NRF_UPSTREAM_ADDRESS_DISCOVERY &&
        d->address <= NRF_UPSTREAM_ADDRESS_REMOTE) {
        ++s_direct_nrf_read_calls;
    }
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
        ++s_nack_read_calls;
        return ESP_ERR_INVALID_STATE; /* IDF 5.5 NACK */
    }
    if (data_len == NRF_CS_CONTROL_RESULT_LENGTH) {
        nrf_cs_control_result_t result;
        uint8_t frame[NRF_CS_CONTROL_RESULT_LENGTH];
        assert(node->cs_status_request_valid);
        assert(node->cs_request_valid);
        assert(nrf_cs_control_step_equal(&node->cs_status_request.step, &node->cs_request.step));
        result.request_id = node->cs_status_request.request_id;
        result.step = node->cs_status_request.step;
        result.state = NRF_CS_CONTROL_STATE_COMPLETE;
        result.command_result = NRF_CS_CONTROL_RESULT_OK;
        result.measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
        result.measurement.distance_mm = 1000 + result.step.pair_index;
        result.measurement.confidence = 900U;
        result.measurement.provider_quality_flags = 0U;
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
        if (s_cs_fail_pair_index == (int32_t)result.step.pair_index) {
            result.state = NRF_CS_CONTROL_STATE_FAILED;
            result.command_result = NRF_CS_CONTROL_RESULT_INTERNAL_ERROR;
            result.measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
            result.measurement.confidence = 0U;
        } else if (s_cs_use_pair_distances) {
            result.measurement.distance_mm = s_cs_pair_distances[result.step.pair_index];
        }
#endif
        if (s_cs_hold_pending) {
            result.state = NRF_CS_CONTROL_STATE_RUNNING;
            result.command_result = NRF_CS_CONTROL_RESULT_PENDING;
        }
        assert(nrf_cs_control_encode_result(&result, frame) == 0);
        if (s_malformed_cs_results_remaining > 0U) {
            frame[42] ^= 0x01U;
            --s_malformed_cs_results_remaining;
            ++s_malformed_cs_results_seen;
        }
        memcpy(data, frame, sizeof(frame));
        ++node->cs_status_reads;
        return ESP_OK;
    }
    nrf_upstream_status_t status;
    assert(nrf_bootstrap_make_status(&node->bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    if (data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        if (s_stop_on_status_read) {
            s_stop_on_status_read = false;
            if (setjmp(s_reconcile_stop_return) == 0) {
                s_reconcile_stop_waiting = true;
                nrf_topology_service_stop();
                assert(false);
            }
            return ESP_ERR_INVALID_STATE;
        }
        if (s_fail_next_status_read) {
            s_fail_next_status_read = false;
            if (s_timeout_failures_until_stuck > 0U) {
                --s_timeout_failures_until_stuck;
            }
            if (s_timeout_failures_until_stuck == 0U) {
                s_scl_stuck = true;
            }
            return ESP_ERR_INVALID_STATE; /* IDF 5.5 timeout/NACK ambiguity */
        }
        if (s_status_response_frame_index < s_status_response_frame_count) {
            assert(s_status_response_frame_index < STATUS_RESPONSE_FRAME_CAPACITY);
            assert(data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
            memcpy(data, s_status_response_frames[s_status_response_frame_index], data_len);
            ++s_status_response_frame_index;
            ++node->status_reads;
            if (s_arm_reselection_probe_after_status_read) {
                s_arm_reselection_probe_after_status_read = false;
                s_reselection_probe_armed = true;
            }
            return ESP_OK;
        }
        ++node->status_reads;
        assert(nrf_upstream_protocol_encode_status(&status, data) == NRF_UPSTREAM_RESULT_OK);
        if (node->delayed_assign_pending) {
            assert(nrf_bootstrap_process_assign(&node->bootstrap, &node->delayed_assign) ==
                   NRF_UPSTREAM_RESULT_OK);
            node->delayed_assign_pending = false;
            ++s_delayed_assignment_confirmations;
        }
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH) {
        nrf_upstream_sensor_snapshot_t sensor;
        memset(&sensor, 0, sizeof(sensor));
        sensor.request_id = node->sensor_request_id;
        sensor.result = node->snapshot_result;
        sensor.valid_mask = node->snapshot_result == NRF_UPSTREAM_SENSOR_RESULT_OK
            ? NRF_UPSTREAM_SENSOR_VALID_MASK
            : 0U;
        sensor.flags = sensor.valid_mask != 0U
            ? NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE
            : 0U;
        sensor.relative_z_mm = node->relative_z_mm;
        sensor.reference_state = node->reference_state;
        sensor.reference_generation = node->reference_generation;
        memcpy(sensor.gyro_bias_mdps, node->gyro_bias_mdps, sizeof(sensor.gyro_bias_mdps));
        memcpy(sensor.gravity_ref_mg, node->gravity_ref_mg, sizeof(sensor.gravity_ref_mg));
        sensor.imu_cal_state = node->imu_cal_state;
        sensor.imu_cal_generation = node->imu_cal_generation;
        assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, data) == NRF_UPSTREAM_RESULT_OK);
        if (node->snapshot_version == 1U) {
            data[30] = 1U;
            data[32] = (uint8_t)NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH;
            data[33] = 0U;
            const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(data, 110U);
            data[110] = (uint8_t)crc;
            data[111] = (uint8_t)(crc >> 8U);
            memset(&data[112], 0xFF, data_len - 112U);
        }
        if (s_malformed_sensor_snapshots_remaining > 0U) {
            data[32] = 0xFFU;
            data[33] = 0xFFU;
            --s_malformed_sensor_snapshots_remaining;
            ++s_malformed_sensor_snapshots_seen;
        }
        return ESP_OK;
    }
    assert(false);
    return ESP_FAIL;
}

esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    if (s_tca_control == 0U && d->address >= NRF_UPSTREAM_ADDRESS_DISCOVERY &&
        d->address <= NRF_UPSTREAM_ADDRESS_REMOTE) {
        ++s_direct_nrf_write_calls;
    }
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_len == NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH &&
        data[0] == 'G' && data[1] == 'S' &&
        data[3] == NRF_LOG_MESSAGE_STATUS_REQUEST) {
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH) {
        nrf_upstream_assign_t assign;
        assert(nrf_upstream_protocol_decode_assign(data, data_len, &assign) == NRF_UPSTREAM_RESULT_OK);
        if (s_delay_next_assignment_confirmation) {
            s_delay_next_assignment_confirmation = false;
            node->delayed_assign = assign;
            node->delayed_assign_pending = true;
        } else {
            (void)nrf_bootstrap_process_assign(&node->bootstrap, &assign);
        }
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH) {
        nrf_upstream_sensor_refresh_t request;
        assert(nrf_upstream_protocol_decode_sensor_refresh(data, data_len, &request) == NRF_UPSTREAM_RESULT_OK);
        node->sensor_request_id = request.request_id;
        ++node->sensor_refreshes;
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH) {
        nrf_upstream_height_reference_t request;
        assert(!s_in_height_calibration_handler);
        assert(node->node_state.role == NRF_NODE_ROLE_REMOTE);
        assert(nrf_upstream_protocol_decode_height_reference(data, data_len, &request) == NRF_UPSTREAM_RESULT_OK);
        assert(request.action == NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE);
        if (s_attempt_height_request_during_capture &&
            !s_height_request_during_capture_attempted) {
            uint8_t requested_mask = 0U;
            uint16_t requested_generation = 0U;
            s_height_request_during_capture_attempted = true;
            s_height_request_during_capture_result =
                nrf_topology_service_request_height_reference_capture(
                    &requested_mask, &requested_generation);
            s_height_request_during_capture_generation = requested_generation;
        }
        assert(node->height_reference_captures <
               sizeof(node->captured_generation) / sizeof(node->captured_generation[0]));
        node->captured_generation[node->height_reference_captures++] = request.generation;
        assert(s_height_reference_writes <
               sizeof(s_height_reference_requests) / sizeof(s_height_reference_requests[0]));
        s_height_reference_requests[s_height_reference_writes++] = request;
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH) {
        nrf_upstream_imu_calibrate_t request;
        assert(!s_in_imu_calibration_handler);
        assert(node->node_state.role == NRF_NODE_ROLE_REMOTE);
        assert(nrf_upstream_protocol_decode_imu_calibrate(data, data_len, &request) ==
               NRF_UPSTREAM_RESULT_OK);
        assert(request.action == NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE);
        assert(node->imu_calibration_requests <
               sizeof(node->imu_calibration_generations) /
                   sizeof(node->imu_calibration_generations[0]));
        node->imu_calibration_generations[node->imu_calibration_requests++] = request.generation;
        assert(s_imu_calibration_writes <
               sizeof(s_imu_calibration_requests) / sizeof(s_imu_calibration_requests[0]));
        s_imu_calibration_requests[s_imu_calibration_writes++] = request;
        return ESP_OK;
    }
    if (data_len == NRF_CS_CONTROL_EXECUTE_LENGTH) {
        nrf_cs_control_request_t request;
        if (nrf_cs_control_decode_execute(data, data_len, &request) == 0) {
            node->cs_request = request;
            node->cs_request_valid = true;
            ++node->cs_execute_writes;
            ++s_cs_execute_count;
            return ESP_OK;
        }
        assert(nrf_cs_control_decode_status_request(data, data_len, &request) == 0);
        node->cs_status_request = request;
        node->cs_status_request_valid = true;
        ++node->cs_status_reads;
        return ESP_OK;
    }
    assert(false);
    return ESP_FAIL;
}

/* sensor_bus platform seam, backed by the same wire. */
static bool fake_line_read_sda(void *context) { (void)context; return true; }
static bool fake_line_read_scl(void *context) { (void)context; return !s_scl_stuck; }
static void fake_line_set(void *context, bool released) { (void)context; (void)released; }
static void fake_line_delay(void *context, uint32_t us) { (void)context; (void)us; }

esp_err_t sensor_bus_lines_bitbang_io(i2c_line_io_t *io)
{
    if (io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *io = (i2c_line_io_t) {
        .read_sda = fake_line_read_sda,
        .read_scl = fake_line_read_scl,
        .set_sda = fake_line_set,
        .set_scl = fake_line_set,
        .delay_us = fake_line_delay,
        .context = NULL,
    };
    return ESP_OK;
}

void sensor_bus_lines_read(bool *sda_high, bool *scl_high)
{
    *sda_high = true;
    *scl_high = !s_scl_stuck;
}

esp_err_t sensor_bus_lines_controller_reset(i2c_bus_handle_t bus)
{
    (void)bus;
    ++s_controller_reset_calls;
    return ESP_OK;
}

esp_err_t sensor_bus_lines_probe(i2c_bus_handle_t bus, uint8_t address)
{
    assert(bus == &s_bus_token);
    ++s_probe_calls;
    if (s_tca_control == 0U && address >= NRF_UPSTREAM_ADDRESS_DISCOVERY &&
        address <= NRF_UPSTREAM_ADDRESS_REMOTE) {
        ++s_direct_nrf_probe_calls;
    }
    if (s_scl_stuck) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_reselection_probe_armed && address == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        s_reselection_probe_armed = false;
        if (s_fail_reselection_probe) {
            s_fail_reselection_probe = false;
            return ESP_ERR_NOT_FOUND;
        }
    }
    return address_acks(address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void sensor_bus_lines_delay_us(uint32_t us)
{
    (void)us;
}

int64_t sensor_bus_lines_now_us(void)
{
    return esp_timer_get_time();
}

/* --------------------------------------------------------------- kernel */

typedef enum { EV_WAIT_MS, EV_ACTION, EV_STOP } event_type_t;
typedef struct {
    event_type_t type;
    uint32_t ms;
    void (*action)(void);
} event_t;

static event_t s_script[256];
static size_t s_script_len;
static size_t s_script_pos;
static int s_critical_depth;
static bool s_in_worker;
static bool s_stop_waiting;
static jmp_buf s_stop_return;
static int s_task_token;
static uint32_t s_random = 0x1234U;

void test_port_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 0);
    ++s_critical_depth;
}

void test_port_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 1);
    --s_critical_depth;
}

struct test_semaphore { int unused; };
static struct test_semaphore s_mutex;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return s_fail_mutex_create ? NULL : &s_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; assert(s_critical_depth == 0); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return s_fail_mutex_create ? NULL : &s_mutex; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t)
{
    (void)s;
    (void)t;
    assert(s_critical_depth == 0);
    if (s_fail_recursive_lock_attempts > 0U) {
        --s_fail_recursive_lock_attempts;
        return pdFALSE;
    }
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

uint32_t esp_random(void) { s_random = s_random * 1103515245U + 12345U; return s_random; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 10000LL + (int64_t)s_fake_subtick_us; }
TickType_t xTaskGetTickCount(void) { return s_tick; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task_token; }
const char *pcTaskGetName(TaskHandle_t task) { (void)task; return "topology-test"; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    if (s_stack_hwm_sequence_pos < s_stack_hwm_sequence_count) {
        return s_stack_hwm_sequence[s_stack_hwm_sequence_pos++];
    }
    return s_stack_hwm_default;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task == &s_task_token); return pdPASS; }

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    assert(s_in_worker);
    assert(s_active_tasks == 1U);
    --s_active_tasks;
    ++s_task_delete_count;
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_critical_depth == 0);
    if (s_stop_waiting) {
        s_stop_waiting = false;
        longjmp(s_stop_return, 1);
    }
    if (s_reconcile_stop_waiting) {
        s_reconcile_stop_waiting = false;
        longjmp(s_reconcile_stop_return, 1);
    }
    s_tick += ticks > 0U ? ticks : 1U;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t stack_depth, void *parameters,
    UBaseType_t priority, TaskHandle_t *created_task, BaseType_t core_id)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    assert(core_id == 0); /* service core */
    ++s_task_create_count;
    if (s_fail_task_create) {
        *created_task = NULL;
        return pdFAIL;
    }
    assert(s_active_tasks == 0U);
    ++s_active_tasks;
    *created_task = &s_task_token;
    s_in_worker = true;
    entry(parameters);
    s_in_worker = false;
    assert(s_active_tasks == 0U);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    (void)clear_on_exit;
    s_last_notify_wait_ms = (uint32_t)(((uint64_t)ticks_to_wait * 1000U) / configTICK_RATE_HZ);
    assert(s_in_worker);
    for (;;) {
        assert(s_script_pos < s_script_len);
        const event_t event = s_script[s_script_pos++];
        if (event.type == EV_ACTION) {
            event.action();
            continue;
        }
        if (event.type == EV_WAIT_MS) {
            s_tick += pdMS_TO_TICKS(event.ms);
            return 1U;
        }
        if (setjmp(s_stop_return) == 0) {
            s_stop_waiting = true;
            nrf_topology_service_stop();
            assert(false);
        }
        return 1U;
    }
}

static void script(event_type_t type, uint32_t ms, void (*action)(void))
{
    assert(s_script_len < sizeof(s_script) / sizeof(s_script[0]));
    s_script[s_script_len].type = type;
    s_script[s_script_len].ms = ms;
    s_script[s_script_len].action = action;
    ++s_script_len;
}

static void script_waits(size_t count, uint32_t ms)
{
    for (size_t i = 0U; i < count; ++i) {
        script(EV_WAIT_MS, ms, NULL);
    }
}

/* ------------------------------------------------------------ scenarios */

static nrf_topology_snapshot_t s_seen;
static nrf_remote_sensor_cache_t s_seen_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static uint8_t s_seen_tca_control;
static nrf_topology_snapshot_t s_initial_topology;
static nrf_remote_sensor_cache_t s_initial_sensor_cache;
static nrf_remote_sensor_cache_t s_unplug_sensor_cache;
static nrf_remote_sensor_cache_t s_rejoin_sensor_cache;
static nrf_remote_sensor_cache_t s_malformed_sensor_cache_before;
static nrf_remote_sensor_cache_t s_malformed_sensor_cache_after;

static void capture(void)
{
    /* Taken while the worker is idle between work items: every channel
     * opened for a reconcile or a sensor refresh must be closed again. */
    s_seen_tca_control = s_tca_control;
    assert(nrf_topology_service_get_snapshot(&s_seen) == ESP_OK);
    for (uint8_t c = 0U; c < NRF_TOPOLOGY_REMOTE_COUNT; ++c) {
        assert(nrf_topology_service_get_sensor_snapshot(c, &s_seen_cache[c]) == ESP_OK);
    }
}

static unsigned assigned_node_count(const nrf_topology_snapshot_t *snapshot)
{
    unsigned assigned = snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assigned += snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    }
    return assigned;
}

static void request_refresh_while_bus_busy(void)
{
    s_fail_recursive_lock_attempts = 1000U;
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void clear_bus_busy_injection(void)
{
    s_fail_recursive_lock_attempts = 0U;
}

static nrf_topology_snapshot_t s_busy_window_snapshot;
static nrf_topology_snapshot_t s_unknown_after_stale_snapshot;
static nrf_topology_snapshot_t s_period_snapshot;
static uint32_t s_period_start_generation;
static unsigned s_sensor_period_start_count;

static void capture_busy_window_snapshot(void)
{
    capture();
    s_busy_window_snapshot = s_seen;
}

static void capture_unknown_after_stale_limit(void)
{
    capture();
    s_unknown_after_stale_snapshot = s_seen;
}

static void capture_period_start(void)
{
    capture();
    s_period_start_generation = s_seen.generation;
    s_sensor_period_start_count = s_nodes[1].sensor_refreshes;
}

static void assert_reconcile_not_due_at_4950ms(void)
{
    capture();
    assert(s_seen.generation == s_period_start_generation);
}

static void assert_reconcile_due_at_5000ms(void)
{
    capture();
    assert(s_seen.generation > s_period_start_generation);
}

static void assert_sensor_refresh_not_due_at_1950ms(void)
{
    assert(s_nodes[1].sensor_refreshes == s_sensor_period_start_count);
}

static void assert_sensor_refresh_due_at_2000ms(void)
{
    assert(s_nodes[1].sensor_refreshes > s_sensor_period_start_count);
}

static void capture_initial_topology(void)
{
    capture();
    s_initial_topology = s_seen;
    s_initial_sensor_cache = s_seen_cache[5];
}

static void unplug_channel5(void)
{
    s_nodes[2].present = false;
}

static void replug_channel5(void)
{
    node_boot(&s_nodes[2], 5U, 0x33U); /* cold node at the discovery address */
}

static nrf_cs_cycle_snapshot_t s_seen_cs;
static nrf_cs_cycle_snapshot_t s_first_cs;
static nrf_topology_snapshot_t s_unplug_snapshot;
static nrf_topology_snapshot_t s_rejoin_snapshot;
static esp_err_t s_height_request_result;
static uint8_t s_height_requested_mask;
static uint16_t s_height_requested_generation;
static esp_err_t s_imu_calibration_request_result;
static uint8_t s_imu_calibration_requested_mask;
static uint16_t s_imu_calibration_requested_generation;

static void request_height_calibration_from_handler(void)
{
    const unsigned writes_before = s_height_reference_writes;
    s_height_requested_mask = 0U;
    s_height_requested_generation = 0U;
    s_in_height_calibration_handler = true;
    s_height_request_result = nrf_topology_service_request_height_reference_capture(
        &s_height_requested_mask, &s_height_requested_generation);
    assert(s_height_reference_writes == writes_before);
    s_in_height_calibration_handler = false;
    assert(s_height_request_result == ESP_OK);
}

static void request_height_calibration_with_no_remotes(void)
{
    const unsigned writes_before = s_height_reference_writes;
    s_height_requested_mask = 0U;
    s_height_requested_generation = 0U;
    s_in_height_calibration_handler = true;
    s_height_request_result = nrf_topology_service_request_height_reference_capture(
        &s_height_requested_mask, &s_height_requested_generation);
    assert(s_height_reference_writes == writes_before);
    s_in_height_calibration_handler = false;
    assert(s_height_request_result == ESP_ERR_NOT_FOUND);
    assert(s_height_requested_mask == 0U);
}

static void request_imu_calibration_from_handler(void)
{
    const unsigned writes_before = s_imu_calibration_writes;
    s_imu_calibration_requested_mask = (uint8_t)((1U << 2U) | (1U << 5U));
    s_imu_calibration_requested_generation = 0xBEEFU;
    s_in_imu_calibration_handler = true;
    assert(nrf_topology_service_request_imu_calibration(0xBEEFU, 0U) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_imu_calibration(0xBEEFU, (uint8_t)(1U << 0U)) ==
           ESP_ERR_NOT_FOUND);
    s_imu_calibration_request_result = nrf_topology_service_request_imu_calibration(
        s_imu_calibration_requested_generation,
        s_imu_calibration_requested_mask);
    assert(s_imu_calibration_writes == writes_before);
    assert(nrf_topology_service_request_imu_calibration(
               s_imu_calibration_requested_generation,
               s_imu_calibration_requested_mask) == ESP_ERR_INVALID_STATE);
    s_in_imu_calibration_handler = false;
    assert(s_imu_calibration_request_result == ESP_OK);
}

static void assert_imu_calibration_requests_and_cached_fields(void)
{
    nrf_remote_sensor_cache_t cache;
    assert(s_imu_calibration_writes == 2U);
    assert(s_nodes[1].imu_calibration_requests == 1U);
    assert(s_nodes[2].imu_calibration_requests == 1U);
    assert(s_nodes[1].imu_calibration_generations[0] == 0xBEEFU);
    assert(s_nodes[2].imu_calibration_generations[0] == 0xBEEFU);
    for (unsigned i = 0U; i < s_imu_calibration_writes; ++i) {
        assert(s_imu_calibration_requests[i].action == NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE);
        assert(s_imu_calibration_requests[i].request_id != 0U);
        assert(s_imu_calibration_requests[i].generation == 0xBEEFU);
    }
    assert(nrf_topology_service_get_sensor_snapshot(5U, &cache) == ESP_OK);
    assert(cache.response_valid);
    assert(cache.sensor.gyro_bias_mdps[0] == -321);
    assert(cache.sensor.gyro_bias_mdps[1] == 456);
    assert(cache.sensor.gravity_ref_mg[1] == 1000);
    assert(cache.sensor.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(cache.sensor.imu_cal_generation == UINT16_MAX);
}

static void assert_height_capture_counts_and_generation_65535(void)
{
    assert(s_height_requested_mask == ((uint8_t)(1U << 2U) | (uint8_t)(1U << 5U)));
    assert(s_height_requested_generation == UINT16_MAX);
    assert(s_height_reference_writes == 2U);
    assert(s_nodes[1].height_reference_captures == 1U);
    assert(s_nodes[2].height_reference_captures == 1U);
    assert(s_nodes[1].captured_generation[0] == UINT16_MAX);
    assert(s_nodes[2].captured_generation[0] == UINT16_MAX);
    assert(s_height_request_during_capture_attempted);
    assert(s_height_request_during_capture_result == ESP_ERR_INVALID_STATE);
    assert(s_height_request_during_capture_generation == 0U);
}

static void assert_height_capture_generation_1(void)
{
    assert(s_height_requested_generation == 1U);
    assert(s_height_reference_writes == 4U);
    assert(s_nodes[1].height_reference_captures == 2U);
    assert(s_nodes[2].height_reference_captures == 2U);
    assert(s_nodes[1].captured_generation[1] == 1U);
    assert(s_nodes[2].captured_generation[1] == 1U);
}

static void assert_height_capture_generation_2(void)
{
    assert(s_height_requested_generation == 2U);
    assert(s_height_reference_writes == 6U);
    assert(s_nodes[1].height_reference_captures == 3U);
    assert(s_nodes[2].height_reference_captures == 3U);
    assert(s_nodes[1].captured_generation[2] == 2U);
    assert(s_nodes[2].captured_generation[2] == 2U);
    for (unsigned index = 0U; index < s_height_reference_writes; ++index) {
        assert(s_height_reference_requests[index].request_id != 0U);
        assert(s_height_reference_requests[index].action == NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE);
    }
}

static void test_height_calibration_worker_queue_and_generation_wrap(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);
    nrf_topology_service_test_set_height_reference_generation((uint16_t)(UINT16_MAX - 1U));
    s_attempt_height_request_during_capture = true;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_height_calibration_from_handler);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_height_capture_counts_and_generation_65535);
    script(EV_ACTION, 0U, request_height_calibration_from_handler);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_height_capture_generation_1);
    script(EV_ACTION, 0U, request_height_calibration_from_handler);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_height_capture_generation_2);
    script(EV_STOP, 0U, NULL);

    run_service();
    assert(s_height_request_result == ESP_OK);
    assert(s_height_requested_mask == ((uint8_t)(1U << 2U) | (uint8_t)(1U << 5U)));
    assert(s_height_reference_writes == 6U);
}

static void test_height_calibration_without_remotes(void)
{
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_height_calibration_with_no_remotes);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_height_reference_writes == 0U);
}

static void test_imu_calibration_worker_dispatch_and_v3_cache(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);
    s_nodes[1].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    s_nodes[2].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    s_nodes[2].gyro_bias_mdps[0] = -321;
    s_nodes[2].gyro_bias_mdps[1] = 456;
    s_nodes[2].gravity_ref_mg[1] = 1000;
    s_nodes[2].imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    s_nodes[2].imu_cal_generation = UINT16_MAX;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_imu_calibration_from_handler);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_imu_calibration_requests_and_cached_fields);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_imu_calibration_request_result == ESP_OK);
    assert(s_imu_calibration_requested_mask ==
           (uint8_t)((1U << 2U) | (1U << 5U)));
    assert(s_imu_calibration_requested_generation == 0xBEEFU);
    assert(s_imu_calibration_writes == 2U);
}

static void capture_cs(void)
{
    assert(nrf_topology_service_get_cs_cycle(&s_seen_cs) == ESP_OK);
}

static void capture_first_cs(void)
{
    capture_cs();
    s_first_cs = s_seen_cs;
}

static void request_cs_cycle(void)
{
    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 1000U,
        .max_attempts = 2U,
        .min_confidence = 500U,
        .samples_per_pair = 1U,
    };
    assert(nrf_topology_service_request_cs_cycle(&policy) == ESP_OK);
}

static unsigned total_status_reads(void)
{
    unsigned total = 0U;
    for (size_t index = 0U; index < MAX_NODES; ++index) {
        total += s_nodes[index].status_reads;
    }
    return total;
}

static void request_cs_during_backoff(void)
{
    s_status_reads_before_backoff = total_status_reads();
    s_cs_hold_pending = true;
    request_cs_cycle();
}

static void assert_reconcile_deferred_during_cs_backoff(void)
{
    nrf_cs_cycle_snapshot_t snapshot;
    assert(nrf_topology_service_get_cs_cycle(&snapshot) == ESP_OK);
    assert(snapshot.active && !snapshot.complete);
    assert(total_status_reads() == s_status_reads_before_backoff);
    assert(s_cs_execute_count > 0U);
    assert(s_tca_control == 0U);
}

#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
static nrf_cs_policy_t ranging_test_policy(void)
{
    return (nrf_cs_policy_t) {
        .pair_timeout_ms = 60000U,
        .max_attempts = 1U,
        .min_confidence = 500U,
        .samples_per_pair = 1U,
        .full_rotation = true,
    };
}

static void prepare_ranging_nodes(size_t remote_count)
{
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    for (size_t index = 0U; index < remote_count; ++index) {
        node_boot(&s_nodes[index + 1U], (uint8_t)index, (uint8_t)(0x22U + index));
        s_nodes[index + 1U].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
        s_nodes[index + 1U].reference_state = NRF_UPSTREAM_REFERENCE_VALID;
        s_nodes[index + 1U].reference_generation = 91U;
    }
    s_device_status.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device_status.main.height_reference.generation = 91U;
    s_device_status.main.height_reference.relative_z_mm = 0;
}

static void request_ranging_with_default_timeout(void)
{
    const nrf_cs_policy_t policy = ranging_test_policy();
    assert(nrf_topology_service_ranging_start(&policy, 0U) == ESP_OK);
}

static void request_ranging_for_test(void)
{
    const nrf_cs_policy_t policy = ranging_test_policy();
    assert(nrf_topology_service_ranging_start(&policy, 1000U) == ESP_OK);
}

static void request_full_ranging_for_test(void)
{
    request_ranging_for_test();
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active && !s_seen_ranging_progress.complete);
    assert(s_seen_ranging_progress.pairs_done == 0U);
    assert(s_seen_ranging_progress.pairs_total == 12U);
    assert(s_seen_ranging_progress.pair_count == 6U);
    assert(s_seen_ranging_progress.pairs[0].node_a == 0U);
    assert(s_seen_ranging_progress.pairs[0].node_b == 1U);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_PENDING);
    assert(s_seen_ranging_progress.geometry.node_count == 4U);
    assert(s_seen_ranging_progress.geometry.nodes[0].node_id == 0U);
    assert(s_seen_ranging_progress.geometry.nodes[0].solved);
    assert(s_seen_ranging_progress.geometry.nodes[0].uncertainty_mm == 0);
    assert(s_seen_ranging_progress.geometry.nodes[1].node_id == 1U);
    assert(!s_seen_ranging_progress.geometry.nodes[1].solved);
    assert(s_seen_ranging_progress.geometry.nodes[1].uncertainty_mm == INT32_MAX);
    assert(s_seen_ranging_progress.solution_residual_mm == INT32_MAX);
}

static void assert_ranging_rejects_missing_main(void)
{
    const nrf_cs_policy_t policy = ranging_test_policy();
    assert(nrf_topology_service_ranging_start(&policy, 1000U) == ESP_ERR_INVALID_STATE);
}

static void assert_ranging_first_direction_progress(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active);
    assert(!s_seen_ranging_progress.complete);
    assert(s_seen_ranging_progress.pairs_done == 1U);
    assert(s_seen_ranging_progress.pairs_total == 12U);
    assert(s_seen_ranging_progress.pair_count == 6U);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_PENDING);
    assert(s_seen_ranging_progress.geometry.node_count == 4U);
    assert(s_seen_ranging_progress.geometry.nodes[0].solved);
    assert(s_seen_ranging_progress.geometry.nodes[0].uncertainty_mm == 0);
    assert(!s_seen_ranging_progress.geometry.nodes[1].solved);
    assert(s_seen_ranging_progress.geometry.nodes[1].uncertainty_mm == INT32_MAX);
    assert(s_seen_ranging_progress.solution_residual_mm == INT32_MAX);
}

static void assert_ranging_pair_zero_progress(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active);
    assert(s_seen_ranging_progress.pairs_done == 2U);
    assert(s_seen_ranging_progress.pairs_total == 12U);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_VALID);
    assert(s_seen_ranging_progress.geometry.node_count == 4U);
    assert(s_seen_ranging_progress.main_height_reference_generation == 91U);
    static const int32_t expected_uncertainty_mm[4] = {
        0, 150, INT32_MAX, INT32_MAX,
    };
    for (size_t node = 0U; node < 4U; ++node) {
        const int32_t uncertainty = s_seen_ranging_progress.geometry.nodes[node].uncertainty_mm;
        assert(uncertainty == expected_uncertainty_mm[node]);
        s_last_uncertainty_mm[node] = uncertainty;
    }
    s_ranging_capture_index = 0U;
}

static void assert_ranging_next_pair_progress(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    ++s_ranging_capture_index;
    const size_t expected_done = 2U * (s_ranging_capture_index + 1U);
    assert(s_seen_ranging_progress.pairs_done == expected_done);
    assert(s_seen_ranging_progress.pairs_total == 12U);
    assert(s_seen_ranging_progress.pair_count == 6U);
    for (size_t pair = 0U; pair <= s_ranging_capture_index; ++pair) {
        assert(s_seen_ranging_progress.pairs[pair].state == NRF_CS_PAIR_VALID);
    }
    for (size_t node = 0U; node < s_seen_ranging_progress.geometry.node_count; ++node) {
        const int32_t uncertainty = s_seen_ranging_progress.geometry.nodes[node].uncertainty_mm;
        assert(uncertainty <= s_last_uncertainty_mm[node]);
        s_last_uncertainty_mm[node] = uncertainty;
    }
    static const int32_t expected_uncertainties[6][4] = {
        {0, 150, INT32_MAX, INT32_MAX},
        {0, 150, INT32_MAX, INT32_MAX},
        {0, 150, INT32_MAX, INT32_MAX},
        {0, 150, 335, INT32_MAX},
        {0, 150, 335, INT32_MAX},
        {0, 140, 254, 249},
    };
    for (size_t node = 0U; node < 4U; ++node) {
        assert(s_seen_ranging_progress.geometry.nodes[node].uncertainty_mm ==
            expected_uncertainties[s_ranging_capture_index][node]);
    }
}

static void assert_ranging_success_frozen(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(!s_seen_ranging_progress.active);
    assert(s_seen_ranging_progress.complete && s_seen_ranging_progress.success);
    assert(s_seen_ranging_progress.failure_reason[0] == '\0');
    assert(s_seen_ranging_progress.pairs_done == 12U);
    assert(s_seen_ranging_progress.pairs_total == 12U);
    assert(s_seen_ranging_progress.pair_count == 6U);
    assert(s_seen_ranging_progress.solution_residual_mm == 0);
    for (size_t pair = 0U; pair < 6U; ++pair) {
        assert(s_seen_ranging_progress.pairs[pair].state == NRF_CS_PAIR_VALID);
        assert(!s_seen_ranging_progress.pairs[pair].single_direction);
        assert(s_seen_ranging_progress.pairs[pair].direction_disagreement_mm == 0);
    }
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_OK);
    assert(s_seen_frozen_ranging_result.available);
    assert(!s_seen_frozen_ranging_result.invalidated);
    assert(s_seen_frozen_ranging_result.membership_identity_valid);
    assert(s_seen_frozen_ranging_result.membership_count == 4U);
    assert(s_seen_frozen_ranging_result.membership[0].channel == 0U);
    assert(s_seen_frozen_ranging_result.membership[0].node_id == 1U);
    assert(s_seen_frozen_ranging_result.membership[3].channel == NRF_UPSTREAM_MUX_CHANNEL_NONE);
    assert(s_seen_frozen_ranging_result.membership[3].node_id == 0U);
    assert(s_seen_frozen_ranging_result.pair_count == 6U);
    assert(s_seen_frozen_ranging_result.topology_generation == s_seen_ranging_progress.topology_generation);
    assert(s_seen_frozen_ranging_result.membership_generation == s_seen_ranging_progress.membership_generation);
    assert(s_seen_frozen_ranging_result.timestamp_ms >= s_seen_ranging_progress.started_at_ms);
    assert(s_seen_frozen_ranging_result.geometry.node_count == 4U);

    nrf_topology_snapshot_t same_membership;
    nrf_topology_ranging_result_t reusable;
    assert(nrf_topology_service_get_snapshot(&same_membership) == ESP_OK);
    ++same_membership.generation;
    const uint64_t frozen_at = s_seen_frozen_ranging_result.timestamp_ms;
    assert(frozen_at > 0U);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &same_membership,
        frozen_at + NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS - 1U,
        NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_OK);
    assert(reusable.available && !reusable.invalidated);
    assert(reusable.topology_generation == s_seen_frozen_ranging_result.topology_generation);

    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &same_membership,
        frozen_at + NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &same_membership,
        frozen_at + NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS + 1U,
        NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &same_membership,
        frozen_at - 1U,
        NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);

    nrf_topology_snapshot_t changed = same_membership;
    changed.remotes[0].hardware_id[0] ^= 1U;
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &changed, frozen_at, NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);

    changed = same_membership;
    changed.remotes[4] = changed.remotes[0];
    changed.remotes[4].mux_channel = 4U;
    changed.remotes[0].state = NRF_TOPOLOGY_NODE_ABSENT;
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &changed, frozen_at, NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);

    changed = same_membership;
    changed.remotes[2].state = NRF_TOPOLOGY_NODE_ABSENT;
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &changed, frozen_at, NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);

    assert(nrf_topology_service_invalidate_ranging() == ESP_OK);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &same_membership, frozen_at, NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS,
        &reusable) == ESP_ERR_NOT_FOUND);
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_OK);
    assert(s_seen_frozen_ranging_result.available && s_seen_frozen_ranging_result.invalidated);
}

static void assert_ranging_pair_failure(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "pair_failed:0-1") == 0);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_ERR_NOT_FOUND);
}

static void assert_ranging_geometry_failure(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "geometry_unsolved:2") == 0);
    for (size_t pair = 0U; pair < s_seen_ranging_progress.pair_count; ++pair) {
        assert(s_seen_ranging_progress.pairs[pair].state == NRF_CS_PAIR_VALID);
    }
}

static void assert_ranging_default_timeout_still_active(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active && !s_seen_ranging_progress.complete);
    assert(s_seen_ranging_progress.pairs_total == 2U);
}

static void assert_ranging_default_timeout_result(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(!s_seen_ranging_progress.active);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "timeout") == 0);
    assert(s_seen_ranging_progress.pairs_done == 2U);
}

static void test_ranging_full_rotation_progress_and_frozen_result(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    s_cs_use_pair_distances = true;
    const int32_t square_distances[6] = {1000, 1000, 1414, 1414, 1000, 1000};
    memcpy(s_cs_pair_distances, square_distances, sizeof(square_distances));

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_full_ranging_for_test);
    script_waits(2U, 10U);
    script(EV_ACTION, 0U, assert_ranging_first_direction_progress);
    script_waits(2U, 10U);
    script(EV_ACTION, 0U, assert_ranging_pair_zero_progress);
    for (size_t pair = 1U; pair < 6U; ++pair) {
        script_waits(4U, 10U);
        script(EV_ACTION, 0U, assert_ranging_next_pair_progress);
    }
    script(EV_ACTION, 0U, assert_ranging_success_frozen);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_cs_execute_count == 12U);
}

static void test_ranging_failure_reasons(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cs_fail_pair_index = 0;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_ranging_for_test);
    script_waits(20U, 10U);
    script(EV_ACTION, 0U, assert_ranging_pair_failure);
    script(EV_STOP, 0U, NULL);
    run_service();

    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cs_use_pair_distances = true;
    s_cs_pair_distances[0] = 1000;
    s_cs_pair_distances[1] = 1000;
    s_cs_pair_distances[2] = 5000;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_ranging_for_test);
    script_waits(12U, 10U);
    script(EV_ACTION, 0U, assert_ranging_geometry_failure);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void test_ranging_default_timeout_is_30_seconds(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(1U);
    s_cs_hold_pending = true;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_ranging_with_default_timeout);
    script(EV_WAIT_MS, 29990U, NULL);
    script(EV_ACTION, 0U, assert_ranging_default_timeout_still_active);
    script(EV_WAIT_MS, 20U, NULL);
    script(EV_ACTION, 0U, assert_ranging_default_timeout_result);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void test_ranging_requires_assigned_main(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[1], 0U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_ranging_rejects_missing_main);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_cs_execute_count == 0U);
}
#endif

static void add_three_remotes_and_refresh(void)
{
    node_boot(&s_nodes[2], 0U, 0x33U);
    node_boot(&s_nodes[3], 1U, 0x44U);
    node_boot(&s_nodes[4], 5U, 0x55U);
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void assert_main_and_four_remotes_are_assigned(void)
{
    assert(nrf_topology_service_get_snapshot(&s_seen) == ESP_OK);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    unsigned assigned_remotes = 0U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assigned_remotes += s_seen.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    }
    assert(assigned_remotes == 4U);
}

static void unplug_channel5_and_refresh(void)
{
    s_nodes[4].present = false;
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void capture_unplug_snapshot(void)
{
    capture();
    s_unplug_snapshot = s_seen;
    s_unplug_sensor_cache = s_seen_cache[5];
}

static void replug_channel5_and_refresh(void)
{
    node_boot(&s_nodes[4], 5U, 0x55U);
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void capture_rejoin_snapshot(void)
{
    capture();
    s_rejoin_snapshot = s_seen;
    s_rejoin_sensor_cache = s_seen_cache[5];
}

static void request_malformed_sensor_snapshot(void)
{
    s_malformed_sensor_snapshots_remaining = 1U;
    assert(nrf_topology_service_request_sensor_refresh(2U) == ESP_OK);
}

static void capture_sensor_cache_before_malformed_snapshot(void)
{
    capture();
    s_malformed_sensor_cache_before = s_seen_cache[2];
}

static void capture_sensor_cache_after_malformed_snapshot(void)
{
    capture();
    s_malformed_sensor_cache_after = s_seen_cache[2];
}

static void test_stack_phase_tracking_and_cs_cycle_path(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    s_malformed_cs_results_remaining = 1U;
    s_stack_hwm_sequence[0] = 1800U; /* first and general reconcile */
    s_stack_hwm_sequence[1] = 1700U; /* initial sensor refresh */
    s_stack_hwm_sequence[2] = 1600U; /* CS cycle start */
    s_stack_hwm_sequence[3] = 900U;  /* first CS pump crosses warning margin */
    s_stack_hwm_sequence[4] = 800U;  /* next CS pump establishes the minimum */
    s_stack_hwm_sequence_count = 5U;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_cs_cycle);
    script_waits(12U, 10U);
    script(EV_ACTION, 0U, capture_first_cs);
    script(EV_ACTION, 0U, add_three_remotes_and_refresh);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_main_and_four_remotes_are_assigned);
    script(EV_ACTION, 0U, request_cs_cycle);
    script_waits(48U, 10U);
    script(EV_ACTION, 0U, capture_cs);
    script(EV_ACTION, 0U, unplug_channel5_and_refresh);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_unplug_snapshot);
    script(EV_ACTION, 0U, replug_channel5_and_refresh);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_rejoin_snapshot);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_first_cs.complete && s_first_cs.pair_count == 1U);
    assert(s_first_cs.pairs[0].state == NRF_CS_PAIR_VALID);
    assert(s_first_cs.pairs[0].attempts >= 3U); /* malformed result was retried */
    assert(s_malformed_cs_results_seen == 1U);
    assert(s_seen_cs.complete && s_seen_cs.pair_count == NRF_CS_MAX_PAIRS);
    for (size_t pair = 0U; pair < s_seen_cs.pair_count; ++pair) {
        assert(s_seen_cs.pairs[pair].state == NRF_CS_PAIR_VALID);
        assert(s_seen_cs.pairs[pair].valid_samples == 1U);
    }
    assert(s_cs_execute_count >= 13U); /* retried 1-node pair plus all 10 pairs in the 5-node cycle */
    assert(s_unplug_snapshot.remotes[5].state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(s_rejoin_snapshot.remotes[5].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_collisions == 0U);

    nrf_topology_service_stack_stats_t stats;
    assert(nrf_topology_service_get_stack_stats(&stats) == ESP_OK);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_FIRST_RECONCILE] == 1800U);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_RECONCILE] == 1800U);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_SENSOR_REFRESH] == 1700U);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_CS_CYCLE_START] == 1600U);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_CS_PUMP_PUBLISH] == 800U);
    assert(stats.minimum_free_bytes == 800U);
    assert((stats.sampled_phase_mask & (1U << NRF_TOPOLOGY_STACK_PHASE_CS_PUMP_PUBLISH)) != 0U);
    assert(s_stack_hwm_warning_logs >= 1U);
    assert(nrf_topology_service_get_stack_stats(NULL) == ESP_ERR_INVALID_ARG);
}

static uint32_t s_timeout_recovery_start;

static void assert_no_line_recovery_yet(void)
{
    sensor_bus_recovery_status_t status;
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.attempts == s_timeout_recovery_start);
}

static void assert_line_recovery_attempted(void)
{
    sensor_bus_recovery_status_t status;
    assert(sensor_bus_get_recovery_status(&status) == ESP_OK);
    assert(status.attempts == s_timeout_recovery_start + 1U);
}

static void request_topology_refresh_for_test(void)
{
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void inject_timeout_before_next_reconcile(void)
{
    s_fail_next_status_read = true;
}

static void test_error_recovery_stack_hwm_and_long_error_name(void)
{
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    s_fail_next_status_read = true;
    s_timeout_failures_until_stuck = 3U;
    memset(s_long_error_name, 'E', sizeof(s_long_error_name) - 1U);
    s_long_error_name[sizeof(s_long_error_name) - 1U] = '\0';
    s_stack_hwm_sequence[0] = 1800U;
    s_stack_hwm_sequence[1] = 1800U;
    s_stack_hwm_sequence[2] = 700U; /* recovery executes on the third timeout window */
    s_stack_hwm_sequence[3] = 1800U;
    s_stack_hwm_sequence_count = 4U;
    sensor_bus_recovery_status_t before;
    assert(sensor_bus_lock_init() == ESP_OK);
    assert(sensor_bus_get_recovery_status(&before) == ESP_OK);
    /* sensor_bus_init performs one boot-time line sample before the worker pass. */
    s_timeout_recovery_start = before.attempts + 1U;
    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, inject_timeout_before_next_reconcile);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, inject_timeout_before_next_reconcile);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_line_recovery_attempted);
    script(EV_STOP, 0U, NULL);
    run_service();

    nrf_topology_service_stack_stats_t stats;
    assert(nrf_topology_service_get_stack_stats(&stats) == ESP_OK);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_ERROR_RECOVERY] == 700U);
    assert(s_stack_hwm_warning_logs >= 1U);
    assert(s_long_error_was_logged);
}

static void test_persistent_timeouts_escalate_after_three_windows(void)
{
    reset_world();
    s_tick = 130000U; /* exceed the line recovery backoff from the prior scenario */
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    s_fail_next_status_read = true;
    s_timeout_failures_until_stuck = 3U;
    assert(sensor_bus_lock_init() == ESP_OK);
    sensor_bus_recovery_status_t before;
    assert(sensor_bus_get_recovery_status(&before) == ESP_OK);
    /* sensor_bus_init performs one boot-time line sample before the worker pass. */
    s_timeout_recovery_start = before.attempts + 1U;

    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, inject_timeout_before_next_reconcile);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, inject_timeout_before_next_reconcile);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_line_recovery_attempted);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.tca_fault);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(s_tca_bus_stuck_logs == 1U);
    assert(s_tca_fault_set_logs == 1U);
}

static size_t live_device_count(void)
{
    size_t live = 0U;
    for (size_t i = 0U; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
        live += s_devices[i].live ? 1U : 0U;
    }
    return live;
}

static void test_lifecycle_failures_mid_reconcile_and_repeated_restart(void)
{
    reset_world();
    s_fail_mutex_create = true;
    assert(sensor_bus_lock_init() == ESP_ERR_NO_MEM);
    s_fail_mutex_create = false;
    assert(sensor_bus_lock_init() == ESP_OK);

    s_fail_task_create = true;
    assert(nrf_topology_service_start() == ESP_ERR_NO_MEM);
    assert(nrf_topology_service_request_refresh() == ESP_ERR_INVALID_STATE);
    nrf_topology_service_stop();
    s_fail_task_create = false;

    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    s_fail_i2c_bus_create = true;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_STOP, 0U, NULL);
    assert(nrf_topology_service_start() == ESP_OK);
    nrf_topology_service_stop();
    assert(!s_bus_live && live_device_count() == 0U);
    assert(sensor_bus_deinit() == ESP_OK);
    s_fail_i2c_bus_create = false;

    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    s_stop_on_status_read = true;
    assert(nrf_topology_service_start() == ESP_OK);
    nrf_topology_service_stop(); /* clear the inline harness's interrupted stop */
    assert(nrf_topology_service_request_refresh() == ESP_ERR_INVALID_STATE);
    s_script_len = 0U;
    s_script_pos = 0U;
    script(EV_STOP, 0U, NULL);
    assert(nrf_topology_service_start() == ESP_OK);
    nrf_topology_service_stop();
    assert(sensor_bus_deinit() == ESP_OK);
    assert(live_device_count() == 0U);

    reset_world();
    s_tca_present = false;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);

    reset_world(); /* restart with an empty wire; the previous node cache must not survive */
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ABSENT);
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assert(!s_seen_cache[channel].identity_valid);
    }

    const unsigned creates_before = s_task_create_count;
    const unsigned deletes_before = s_task_delete_count;
    for (unsigned iteration = 0U; iteration < 100U; ++iteration) {
        reset_world();
        script(EV_STOP, 0U, NULL);
        run_service();
    }
    assert(s_task_create_count - creates_before == 100U);
    assert(s_task_delete_count - deletes_before == 100U);
    assert(s_active_tasks == 0U);
}

static void reset_world(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_height_reference_requests, 0, sizeof(s_height_reference_requests));
    memset(s_imu_calibration_requests, 0, sizeof(s_imu_calibration_requests));
    s_tca_present = false;
    s_tca_control = 0U;
    s_tca_writes = 0U;
    s_allow_address_collisions = false;
    s_collisions = 0U;
    s_direct_nrf_read_calls = 0U;
    s_direct_nrf_write_calls = 0U;
    s_direct_nrf_probe_calls = 0U;
    s_controller_reset_calls = 0U;
    s_fail_i2c_bus_create = false;
    s_fail_mutex_create = false;
    s_fail_task_create = false;
    s_fail_next_status_read = false;
    s_timeout_failures_until_stuck = 0U;
    memset(s_status_response_frames, 0, sizeof(s_status_response_frames));
    s_status_response_frame_count = 0U;
    s_status_response_frame_index = 0U;
    s_arm_reselection_probe_after_status_read = false;
    s_reselection_probe_armed = false;
    s_fail_reselection_probe = false;
    s_nack_read_calls = 0U;
    s_nack_byte_read_calls = 0U;
    s_probe_calls = 0U;
    s_scl_stuck = false;
    s_stop_on_status_read = false;
    s_reconcile_stop_waiting = false;
    s_malformed_cs_results_remaining = 0U;
    s_malformed_cs_results_seen = 0U;
    s_malformed_sensor_snapshots_remaining = 0U;
    s_malformed_sensor_snapshots_seen = 0U;
    s_delay_next_assignment_confirmation = false;
    s_delayed_assignment_confirmations = 0U;
    s_cs_execute_count = 0U;
    s_in_height_calibration_handler = false;
    s_attempt_height_request_during_capture = false;
    s_height_request_during_capture_attempted = false;
    s_height_request_during_capture_result = ESP_ERR_INVALID_STATE;
    s_height_request_during_capture_generation = 0U;
    s_height_reference_writes = 0U;
    s_in_imu_calibration_handler = false;
    s_imu_calibration_writes = 0U;
    s_cs_hold_pending = false;
    s_status_reads_before_backoff = 0U;
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
    s_cs_use_pair_distances = false;
    memset(s_cs_pair_distances, 0, sizeof(s_cs_pair_distances));
    s_cs_fail_pair_index = -1;
    memset(&s_device_status, 0, sizeof(s_device_status));
    memset(&s_seen_ranging_progress, 0, sizeof(s_seen_ranging_progress));
    memset(&s_seen_frozen_ranging_result, 0, sizeof(s_seen_frozen_ranging_result));
    memset(s_last_uncertainty_mm, 0, sizeof(s_last_uncertainty_mm));
    s_ranging_capture_index = 0U;
#endif
    s_height_request_result = ESP_ERR_INVALID_STATE;
    s_height_requested_mask = 0U;
    s_imu_calibration_request_result = ESP_ERR_INVALID_STATE;
    s_imu_calibration_requested_mask = 0U;
    s_imu_calibration_requested_generation = 0U;
    s_stack_hwm_default = 2048U;
    s_stack_hwm_sequence_count = 0U;
    s_stack_hwm_sequence_pos = 0U;
    s_stack_hwm_warning_logs = 0U;
    s_long_error_name[0] = '\0';
    s_long_error_was_logged = false;
    s_tca_fault_set_logs = 0U;
    s_tca_bus_stuck_logs = 0U;
    s_script_len = 0U;
    s_script_pos = 0U;
    s_tick = 1000U;
    s_fake_subtick_us = 0U;
    s_fake_transaction_duration_us = 0U;
    s_fail_recursive_lock_attempts = 0U;
    s_last_notify_wait_ms = 0U;
    s_max_bus_owner_hold_ms = 0U;
    s_bus_owner_log_count = 0U;
    s_critical_depth = 0;
    s_in_worker = false;
    s_stop_waiting = false;
    assert(s_active_tasks == 0U);
}

static void run_service(void)
{
    assert(sensor_bus_lock_init() == ESP_OK);
    assert(nrf_topology_service_start() == ESP_OK);
    /* The worker ran inline and was stopped by the script; finish the
     * interrupted stop() bookkeeping (task already gone). */
    nrf_topology_service_stop();
    /* Every device handle the service created is gone after stop. */
    size_t live = 0U;
    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
        live += s_devices[i].live ? 1U : 0U;
    }
    /* Only sensor_bus-owned handles (INA3221 and, if found, the TCA). */
    assert(live == (s_tca_present ? 2U : 1U));
    (void)sensor_bus_deinit();
}

static void test_reconcile_deferred_during_recording_start_backoff(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 0U, 0x22U);
    /* A pending CS status response models the recording-start backoff. This
     * guard uses the common CS cycle API and therefore runs without the
     * ranging-only fixture and result types. */
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_cs_during_backoff);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_reconcile_deferred_during_cs_backoff);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void prepare_direct_status_read_case(void)
{
    sensor_bus_handles_t handles;
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    assert(sensor_bus_lock_init() == ESP_OK);
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
}

static void queue_log_response(uint8_t message_type)
{
    uint8_t *frame;
    uint16_t frame_length;
    uint16_t crc;
    assert(s_status_response_frame_count < STATUS_RESPONSE_FRAME_CAPACITY);
    assert(message_type == NRF_LOG_MESSAGE_STATUS || message_type == NRF_LOG_MESSAGE_BLOCK);
    frame_length = message_type == NRF_LOG_MESSAGE_STATUS
        ? NRF_LOG_PROTOCOL_STATUS_LENGTH
        : NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH;
    assert(frame_length <= sizeof(s_status_response_frames[0]));
    frame = s_status_response_frames[s_status_response_frame_count];
    memset(frame, 0, frame_length);
    frame[0] = 'G';
    frame[1] = 'S';
    frame[2] = NRF_LOG_PROTOCOL_VERSION;
    frame[3] = message_type;
    if (message_type == NRF_LOG_MESSAGE_STATUS) {
        frame[10] = NRF_LOG_PROTOCOL_VERSION;
        frame[12] = 128U;
        frame[16] = 1U;
    } else {
        frame[10] = NRF_LOG_PROTOCOL_VERSION;
        frame[14] = 1U;
    }
    frame[4] = (uint8_t)frame_length;
    frame[5] = (uint8_t)(frame_length >> 8U);
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, frame_length - NRF_LOG_PROTOCOL_CRC_LENGTH);
    frame[frame_length - NRF_LOG_PROTOCOL_CRC_LENGTH] = (uint8_t)crc;
    frame[frame_length - 1U] = (uint8_t)(crc >> 8U);
    ++s_status_response_frame_count;
}

static void queue_upstream_status_response(bool corrupt_crc)
{
    nrf_upstream_status_t status;
    uint8_t *frame;
    assert(s_status_response_frame_count < STATUS_RESPONSE_FRAME_CAPACITY);
    assert(nrf_bootstrap_make_status(&s_nodes[0].bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    frame = s_status_response_frames[s_status_response_frame_count];
    assert(nrf_upstream_protocol_encode_status(&status, frame) == NRF_UPSTREAM_RESULT_OK);
    if (corrupt_crc) {
        frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH - 1U] ^= 0x01U;
    }
    ++s_status_response_frame_count;
}

static esp_err_t read_cached_status(unsigned *read_count, unsigned *probe_count)
{
    nrf_upstream_status_t status;
    const unsigned reads_before = s_nodes[0].status_reads;
    const unsigned probes_before = s_probe_calls;
    const esp_err_t err = nrf_topology_service_test_read_status_cached_probe(
        NRF_UPSTREAM_ADDRESS_DISCOVERY, &status);
    *read_count = s_nodes[0].status_reads - reads_before;
    *probe_count = s_probe_calls - probes_before;
    return err;
}

static void test_stale_log_response_reselects_status(void)
{
    unsigned read_count;
    unsigned probe_count;

    prepare_direct_status_read_case();
    queue_log_response(NRF_LOG_MESSAGE_STATUS);
    queue_upstream_status_response(false);
    s_arm_reselection_probe_after_status_read = true;
    assert(read_cached_status(&read_count, &probe_count) == ESP_OK);
    assert(read_count == 2U);
    assert(probe_count == 1U);
    assert(s_status_response_frame_index == 2U);
    assert(sensor_bus_deinit() == ESP_OK);

    prepare_direct_status_read_case();
    queue_log_response(NRF_LOG_MESSAGE_STATUS);
    queue_log_response(NRF_LOG_MESSAGE_BLOCK);
    s_arm_reselection_probe_after_status_read = true;
    assert(read_cached_status(&read_count, &probe_count) == ESP_ERR_INVALID_RESPONSE);
    assert(read_count == 2U);
    assert(probe_count == 1U);
    assert(s_status_response_frame_index == 2U);
    assert(sensor_bus_deinit() == ESP_OK);

    prepare_direct_status_read_case();
    assert(read_cached_status(&read_count, &probe_count) == ESP_OK);
    assert(read_count == 1U);
    assert(probe_count == 0U);
    assert(sensor_bus_deinit() == ESP_OK);

    prepare_direct_status_read_case();
    queue_upstream_status_response(true);
    assert(read_cached_status(&read_count, &probe_count) == ESP_ERR_INVALID_RESPONSE);
    assert(read_count == 1U);
    assert(probe_count == 0U);
    assert(sensor_bus_deinit() == ESP_OK);

    prepare_direct_status_read_case();
    queue_log_response(NRF_LOG_MESSAGE_STATUS);
    s_arm_reselection_probe_after_status_read = true;
    s_fail_reselection_probe = true;
    assert(read_cached_status(&read_count, &probe_count) == ESP_ERR_NOT_FOUND);
    assert(read_count == 1U);
    assert(probe_count == 1U);
    assert(s_status_response_frame_index == 1U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_full_topology_is_assigned_one_hot(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U); /* MAIN, in the enclosure */
    node_boot(&s_nodes[1], 2U, 0x22U);               /* REMOTE on channel 2 */
    node_boot(&s_nodes[2], 5U, 0x33U);               /* REMOTE on channel 5 */

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.tca_available && !s_seen.tca_fault);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[5].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    for (uint8_t c = 0U; c < NRF_TOPOLOGY_REMOTE_COUNT; ++c) {
        if (c != 2U && c != 5U) {
            /* MAX-248: an empty channel NACKs; that is "absent". */
            assert(s_seen.remotes[c].state == NRF_TOPOLOGY_NODE_ABSENT);
        }
    }
    /* Nodes moved to their runtime addresses. */
    assert(s_nodes[0].address == NRF_UPSTREAM_ADDRESS_MAIN);
    assert(s_nodes[1].address == NRF_UPSTREAM_ADDRESS_REMOTE);
    assert(s_nodes[2].address == NRF_UPSTREAM_ADDRESS_REMOTE);
    assert(s_seen.remotes[2].node_id != s_seen.remotes[5].node_id);
    assert(s_seen.remotes[2].node_id != s_seen.main.node_id);
    assert(s_probe_calls > 0U);
    assert(s_nack_read_calls == 0U); /* absent channels are probe-only */
    assert(s_nack_byte_read_calls == 0U); /* absent mux candidates are probe-only */

    /* The initial sensor refresh reached both remotes through the mux. */
    assert(s_nodes[1].sensor_refreshes >= 1U && s_nodes[2].sensor_refreshes >= 1U);
    assert(s_seen_cache[2].state == NRF_SENSOR_CACHE_NO_DATA);
    assert(s_seen_cache[5].state == NRF_SENSOR_CACHE_NO_DATA);
    /* Two REMOTEs share 0x32 and never answered together (asserted on
     * every transfer); the mux is closed whenever the worker is idle. */
    assert(s_collisions == 0U);
    assert(s_seen_tca_control == 0x00U);
    assert(s_tca_control == 0x00U);
    /* The mux was really driven (select + close per channel). */
    assert(s_tca_writes >= 2U * NRF_TOPOLOGY_REMOTE_COUNT);
}

static void test_mux_main_address_conflict_blocks_direct_nrf_io(void)
{
    static const uint8_t addresses[] = {
        NRF_UPSTREAM_ADDRESS_MAIN,
        NRF_UPSTREAM_ADDRESS_DISCOVERY,
        NRF_UPSTREAM_ADDRESS_REMOTE,
    };
    reset_world();
    s_tca_present = true;
    s_allow_address_collisions = true;
    /* A pod on ch0 answers at MAIN's direct address with a different identity. */
    node_boot(&s_nodes[0], 0U, 0x22U);
    s_nodes[0].address = NRF_UPSTREAM_ADDRESS_MAIN;
    node_boot(&s_nodes[1], UPSTREAM_CHANNEL, 0x11U);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(s_collisions > 0U);
    /* Discovery status plus assignment confirmation ran directly before the
     * channel exposed the conflict; there is no follow-up direct MAIN read. */
    assert(s_direct_nrf_read_calls == 2U);
    assert(s_direct_nrf_write_calls == 1U);

    s_direct_nrf_read_calls = 0U;
    s_direct_nrf_write_calls = 0U;
    s_direct_nrf_probe_calls = 0U;
    for (size_t index = 0U; index < sizeof(addresses); ++index) {
        assert(nrf_topology_service_test_direct_nrf_io(
            addresses[index], false) == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET);
        assert(nrf_topology_service_test_direct_nrf_io(
            addresses[index], true) == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET);
    }
    assert(s_direct_nrf_read_calls == 0U);
    assert(s_direct_nrf_write_calls == 0U);
    assert(s_direct_nrf_probe_calls == 0U);
}

static void test_assignment_confirmation_race_preserves_node_mapping(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);
    s_delay_next_assignment_confirmation = true;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_delayed_assignment_confirmations == 1U);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[5].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.main.node_id == NRF_TOPOLOGY_MAIN_NODE_ID);
    assert(s_seen.remotes[2].node_id == 3U && s_seen.remotes[5].node_id == 6U);
    assert(memcmp(s_seen.main.hardware_id, s_nodes[0].hardware_id,
                  sizeof(s_nodes[0].hardware_id)) == 0);
    assert(memcmp(s_seen.remotes[2].hardware_id, s_nodes[1].hardware_id,
                  sizeof(s_nodes[1].hardware_id)) == 0);
    assert(memcmp(s_seen.remotes[5].hardware_id, s_nodes[2].hardware_id,
                  sizeof(s_nodes[2].hardware_id)) == 0);
    assert(s_seen.main.assignment_request_id != 0U);
    assert(s_seen.remotes[2].assignment_request_id != s_seen.main.assignment_request_id);
    assert(s_seen.remotes[5].assignment_request_id != s_seen.remotes[2].assignment_request_id);
    assert(s_collisions == 0U && s_tca_control == 0x00U);
}

static void test_remote_snapshot_v1_and_v2_cache_read_path(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);
    s_nodes[1].snapshot_version = 1U;
    s_nodes[1].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    s_nodes[2].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    s_nodes[2].relative_z_mm = -2467;
    s_nodes[2].reference_state = NRF_UPSTREAM_REFERENCE_VALID;
    s_nodes[2].reference_generation = 91U;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen_cache[2].response_valid);
    assert(s_seen_cache[2].sensor.relative_z_mm == 0);
    assert(s_seen_cache[2].sensor.reference_state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(s_seen_cache[2].sensor.reference_generation == 0U);
    assert(s_seen_cache[5].response_valid);
    assert(s_seen_cache[5].sensor.relative_z_mm == -2467);
    assert(s_seen_cache[5].sensor.reference_state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(s_seen_cache[5].sensor.reference_generation == 91U);
}

/* MAX-434 B4: all eight channels reconcile and channel 0 receives refresh. */
static void test_topology_and_sensor_refresh_holds_stay_within_budget(void)
{
    reset_world();
    s_tca_present = true;
    s_fake_transaction_duration_us = SENSOR_BUS_TRANSACTION_TIMEOUT_MS * 1000U;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        node_boot(&s_nodes[channel + 1U], channel, (uint8_t)(0x20U + channel));
    }

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assert(s_seen.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(s_nodes[channel + 1U].sensor_refreshes >= 1U);
    }
    assert(s_bus_owner_log_count >= NRF_TOPOLOGY_REMOTE_COUNT);
    assert(s_max_bus_owner_hold_ms <= SENSOR_BUS_OWNER_MAX_HOLD_MS);
    assert(s_seen.tca_available && !s_seen.tca_fault && !s_seen.bus_busy);
    assert(s_tca_control == 0U && s_collisions == 0U);
}

static void assert_initial_explicit_refreshes_complete(void)
{
    assert(s_nodes[1].sensor_refreshes == 1U);
    assert(s_nodes[2].sensor_refreshes == 1U);
}

static void queue_explicit_refresh_mask_while_busy(void)
{
    const uint8_t mask = 0x24U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if ((mask & (uint8_t)(1U << channel)) != 0U) {
            assert(nrf_topology_service_request_sensor_refresh(channel) == ESP_OK);
        }
    }
    request_refresh_while_bus_busy();
}

static void assert_explicit_refreshes_wait_for_reconcile(void)
{
    capture_busy_window_snapshot();
    assert(s_busy_window_snapshot.bus_busy && s_busy_window_snapshot.stale);
    assert(s_nodes[1].sensor_refreshes == 1U);
    assert(s_nodes[2].sensor_refreshes == 1U);
}

static void request_successful_reconcile(void)
{
    clear_bus_busy_injection();
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

/* MAX-434 B5: explicit bits 2 and 5 survive a busy window until refresh. */
static void test_sensor_refresh_requests_survive_busy_reconcile(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_initial_explicit_refreshes_complete);
    script(EV_ACTION, 0U, queue_explicit_refresh_mask_while_busy);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_explicit_refreshes_wait_for_reconcile);
    script(EV_ACTION, 0U, request_successful_reconcile);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[5].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_nodes[1].sensor_refreshes == 2U);
    assert(s_nodes[2].sensor_refreshes == 2U);
    assert(!s_seen.bus_busy && !s_seen.stale);
    assert(s_tca_control == 0U && s_collisions == 0U);
}

static void test_busy_window_preserves_last_good_snapshot(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_initial_topology);
    script(EV_ACTION, 0U, request_refresh_while_bus_busy);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_busy_window_snapshot);
    script(EV_ACTION, 0U, clear_bus_busy_injection);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_busy_window_snapshot.generation == s_initial_topology.generation + 1U);
    assert(s_busy_window_snapshot.bus_busy && s_busy_window_snapshot.stale);
    assert(s_busy_window_snapshot.age_ms < NRF_TOPOLOGY_STALE_LIMIT_MS);
    assert(assigned_node_count(&s_busy_window_snapshot) == 2U);
}

static void arm_bus_busy_injection(void)
{
    s_fail_recursive_lock_attempts = 1000U;
}

static void clear_bus_busy_and_capture(void)
{
    clear_bus_busy_injection();
    capture();
    s_busy_window_snapshot = s_seen;
}

static void test_snapshot_becomes_unknown_after_stale_limit(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_initial_topology);
    script(EV_ACTION, 0U, request_refresh_while_bus_busy);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_busy_window_snapshot);
    script(EV_ACTION, 0U, clear_bus_busy_injection);
    for (unsigned period = 0U; period < 5U; ++period) {
        script(EV_ACTION, 0U, arm_bus_busy_injection);
        script(EV_WAIT_MS, NRF_TOPOLOGY_RECONCILE_PERIOD_MS, NULL);
        script(EV_ACTION, 0U, clear_bus_busy_and_capture);
    }
    script(EV_ACTION, 0U, capture_unknown_after_stale_limit);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_busy_window_snapshot.bus_busy && s_busy_window_snapshot.stale);
    assert(s_busy_window_snapshot.age_ms >= NRF_TOPOLOGY_STALE_LIMIT_MS);
    assert(s_unknown_after_stale_snapshot.main.state == NRF_TOPOLOGY_NODE_UNKNOWN);
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assert(s_unknown_after_stale_snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_UNKNOWN);
    }
    assert(assigned_node_count(&s_unknown_after_stale_snapshot) == 0U);
}

static void test_named_reconcile_and_sensor_periods_on_fake_clock(void)
{
    assert(NRF_TOPOLOGY_RECONCILE_PERIOD_MS == 5000U);
    assert(NRF_SENSOR_REFRESH_PERIOD_MS == 2000U);
    assert(NRF_LOG_COLLECTOR_PERIOD_MS == 1000U);
    assert(NRF_CS_PUMP_INTERVAL_MS == 10U);
    assert(NRF_TOPOLOGY_PERIOD_TOLERANCE_MS == 50U);
    assert(NRF_CS_PUMP_TOLERANCE_MS == 2U);

    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_period_start);
    script(EV_WAIT_MS, 4940U, NULL);
    script(EV_ACTION, 0U, assert_reconcile_not_due_at_4950ms);
    script(EV_WAIT_MS, 50U, NULL);
    script(EV_ACTION, 0U, assert_reconcile_due_at_5000ms);
    script(EV_STOP, 0U, NULL);
    run_service();

    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_period_start);
    script(EV_WAIT_MS, 1940U, NULL);
    script(EV_ACTION, 0U, assert_sensor_refresh_not_due_at_1950ms);
    script(EV_WAIT_MS, 50U, NULL);
    script(EV_ACTION, 0U, assert_sensor_refresh_due_at_2000ms);
    script(EV_STOP, 0U, NULL);
    run_service();
}

#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
static void assert_cs_wait_uses_named_10ms_period(void)
{
    nrf_cs_cycle_snapshot_t snapshot;
    assert(s_last_notify_wait_ms >= NRF_CS_PUMP_INTERVAL_MS - NRF_CS_PUMP_TOLERANCE_MS);
    assert(s_last_notify_wait_ms <= NRF_CS_PUMP_INTERVAL_MS + NRF_CS_PUMP_TOLERANCE_MS);
    assert(nrf_topology_service_get_cs_cycle(&snapshot) == ESP_OK);
    assert(snapshot.active && !snapshot.complete);
}

static void test_cs_pump_period_on_fake_clock(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(1U);
    s_cs_hold_pending = true;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_cs_cycle);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_cs_wait_uses_named_10ms_period);
    script(EV_STOP, 0U, NULL);
    run_service();
}
#endif

static void test_malformed_snapshot_marks_cached_sample_stale(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    s_nodes[1].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    s_nodes[1].relative_z_mm = -1350;
    s_nodes[1].reference_state = NRF_UPSTREAM_REFERENCE_VALID;
    s_nodes[1].reference_generation = 12U;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_sensor_cache_before_malformed_snapshot);
    script(EV_ACTION, 0U, request_malformed_sensor_snapshot);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_sensor_cache_after_malformed_snapshot);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_malformed_sensor_snapshots_seen == 1U);
    assert(s_malformed_sensor_cache_before.response_valid);
    assert(s_malformed_sensor_cache_before.topology_current);
    assert(s_malformed_sensor_cache_before.sensor.relative_z_mm == -1350);
    assert(s_malformed_sensor_cache_after.state == NRF_SENSOR_CACHE_PROTOCOL_ERROR);
    assert(s_malformed_sensor_cache_after.last_attempt_state == NRF_SENSOR_CACHE_PROTOCOL_ERROR);
    assert(!s_malformed_sensor_cache_after.topology_current);
    /* Keep the previous sample as history, but mark it unusable for the current topology. */
    assert(s_malformed_sensor_cache_after.response_valid);
    assert(s_malformed_sensor_cache_after.sensor.relative_z_mm == -1350);
    assert(s_tca_control == 0x00U);
}

static void test_without_tca_only_main_is_managed(void)
{
    reset_world();
    s_tca_present = false;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(!s_seen.tca_available);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_nodes[0].address == NRF_UPSTREAM_ADDRESS_MAIN);
    for (uint8_t c = 0U; c < NRF_TOPOLOGY_REMOTE_COUNT; ++c) {
        assert(s_seen.remotes[c].state != NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(s_seen.remotes[c].state != NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    }
}

static void test_unplug_and_replug_between_reconciles(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_initial_topology);
    script(EV_ACTION, 0U, unplug_channel5);
    script(EV_WAIT_MS, 5000U, NULL);    /* periodic reconcile */
    script(EV_ACTION, 0U, capture_unplug_snapshot);
    script(EV_ACTION, 0U, replug_channel5);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_rejoin_snapshot);
    script(EV_STOP, 0U, NULL);

    assert(sensor_bus_lock_init() == ESP_OK);
    assert(nrf_topology_service_start() == ESP_OK);
    nrf_topology_service_stop();

    /* After the unplug the channel read as absent, not as a fault. */
    assert(s_unplug_snapshot.remotes[5].state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(s_unplug_snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_initial_sensor_cache.identity_valid && s_initial_sensor_cache.topology_current);
    assert(s_unplug_snapshot.generation > s_initial_topology.generation);
    assert(s_rejoin_snapshot.generation > s_unplug_snapshot.generation);
    assert(!s_unplug_sensor_cache.identity_valid);
    assert(!s_unplug_sensor_cache.response_valid);
    assert(s_rejoin_snapshot.remotes[5].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_rejoin_snapshot.remotes[5].assignment_request_id !=
           s_initial_topology.remotes[5].assignment_request_id);
    assert(s_rejoin_sensor_cache.identity_valid && s_rejoin_sensor_cache.topology_current);
    /* A periodic reconcile without sensor work also leaves the mux closed. */
    assert(s_seen_tca_control == 0x00U);
    /* The re-plugged cold node was found at 0x30 and assigned again. */
    assert(s_nodes[2].address == NRF_UPSTREAM_ADDRESS_REMOTE);
    assert(s_collisions == 0U);
    assert(s_tca_control == 0x00U);
    (void)sensor_bus_deinit();
}

static void test_api_guards_when_stopped(void)
{
    nrf_topology_snapshot_t snapshot;
    nrf_cs_policy_t ranging_policy = {
        .pair_timeout_ms = 100U,
        .max_attempts = 1U,
        .min_confidence = 500U,
        .full_rotation = true,
    };
    nrf_topology_ranging_progress_t ranging_progress;
    nrf_topology_ranging_result_t ranging_result;
    uint8_t height_mask = UINT8_MAX;
    uint16_t height_generation = UINT16_MAX;
    assert(nrf_topology_service_request_refresh() == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_sensor_refresh(NRF_TOPOLOGY_REMOTE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_height_reference_capture(
        &height_mask, &height_generation) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_imu_calibration(1U, 0U) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_imu_calibration(1U, 1U) == ESP_ERR_INVALID_STATE);
    assert(height_mask == 0U);
    assert(height_generation == 0U);
    assert(nrf_topology_service_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_start(NULL, 0U) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_start(&ranging_policy, 0U) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_ranging_get_progress(NULL) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_get_progress(&ranging_progress) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_get_frozen_ranging_result(NULL) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_get_frozen_ranging_result(&ranging_result) == ESP_ERR_NOT_FOUND);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        NULL, 0U, 0U, &ranging_result) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_invalidate_ranging() == ESP_OK);
    (void)snapshot;
    /* stop() on a stopped service returns immediately. */
    nrf_topology_service_stop();
}

int main(void)
{
    test_api_guards_when_stopped();
    test_lifecycle_failures_mid_reconcile_and_repeated_restart();
    test_stale_log_response_reselects_status();
    test_assignment_confirmation_race_preserves_node_mapping();
    test_malformed_snapshot_marks_cached_sample_stale();
    test_unplug_and_replug_between_reconciles();
    test_full_topology_is_assigned_one_hot();
    test_mux_main_address_conflict_blocks_direct_nrf_io();
    test_remote_snapshot_v1_and_v2_cache_read_path();
    test_height_calibration_worker_queue_and_generation_wrap();
    test_height_calibration_without_remotes();
    test_imu_calibration_worker_dispatch_and_v3_cache();
    test_without_tca_only_main_is_managed();
    test_stack_phase_tracking_and_cs_cycle_path();
    test_error_recovery_stack_hwm_and_long_error_name();
    test_persistent_timeouts_escalate_after_three_windows();
    test_reconcile_deferred_during_recording_start_backoff();
    test_topology_and_sensor_refresh_holds_stay_within_budget();
    test_sensor_refresh_requests_survive_busy_reconcile();
    test_busy_window_preserves_last_good_snapshot();
    test_snapshot_becomes_unknown_after_stale_limit();
    test_named_reconcile_and_sensor_periods_on_fake_clock();
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
    test_cs_pump_period_on_fake_clock();
    test_ranging_full_rotation_progress_and_frozen_result();
    test_ranging_failure_reasons();
    test_ranging_default_timeout_is_30_seconds();
    test_ranging_requires_assigned_main();
#endif
    return 0;
}
