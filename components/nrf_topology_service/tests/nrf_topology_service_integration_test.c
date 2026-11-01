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
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "nrf_bootstrap.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_cs_cycle_runtime.h"
#include "nrf_log_protocol.h"
#include "nrf_node_state.h"
#include "nrf_read_window.h"
#include "nrf_topology_service.h"
#include "nrf_topology_periods.h"
#include "nrf_upstream_protocol.h"
#include "sensor_bus.h"
#include "sensor_bus_lines.h"
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
#include "device_status.h"
#include "main_barometer_fake.h"
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
    unsigned sensor_scans;
    uint32_t sensor_scan_id;
    unsigned cs_execute_writes;
    unsigned cs_status_reads;
    /* The real nRF keeps a copy of the response it last handed out and serves read windows from it. */
    nrf_read_window_target_t window;
    bool cs_result_selected;
    /* Autonomous ranging: the node side of the cycle protocol, driven by the fake clock. */
    nrf_cs_cycle_runtime_t cycle;
    uint8_t cycle_reply[NRF_CS_CYCLE_RESULT_LENGTH];
    bool cycle_reply_selected;
    unsigned cycle_plan_writes;
    unsigned cycle_go_writes;
    unsigned cycle_ble_attempts;
} sim_node_t;

typedef struct {
    uint8_t address;
    bool live;
    uint32_t scl_wait_us;
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
static bool s_native_bus_live;
static uint32_t s_tick;
static uint64_t s_fake_subtick_us;
static uint32_t s_fake_transaction_duration_us;
static void (*s_vtask_delay_hook)(TickType_t ticks);
static unsigned s_fail_recursive_lock_attempts;
static bool s_block_remote_scan_after_main_status;
static uint32_t s_last_notify_wait_ms;
static uint32_t s_max_bus_owner_hold_ms;
static unsigned s_bus_owner_log_count;
static unsigned s_collisions;
static unsigned s_direct_nrf_read_calls;
static unsigned s_direct_nrf_write_calls;
static unsigned s_window_writes;
static unsigned s_window_reads;
static unsigned s_direct_nrf_probe_calls;
static unsigned s_controller_reset_calls;
static bool s_fail_i2c_bus_create;
static bool s_fail_mutex_create;
static bool s_fail_task_create;
static bool s_fail_next_status_read;
static bool s_fail_next_main_status_read;
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
static bool s_log_reply_selected;
static jmp_buf s_reconcile_stop_return;
static unsigned s_malformed_cs_results_remaining;
static unsigned s_malformed_cs_results_seen;
static unsigned s_malformed_sensor_snapshots_remaining;
static unsigned s_malformed_sensor_snapshots_seen;
static bool s_delay_next_assignment_confirmation;
static unsigned s_delayed_assignment_confirmations;
static unsigned s_cs_execute_count;
static uint8_t s_fail_cs_write_type;
static uint8_t s_fail_cs_write_channel;
static unsigned s_failed_cs_writes;
static uint32_t s_cs_recovery_attempts_before;
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
/* Autonomous ranging scenarios. */
typedef struct {
    int64_t at_us;
    uint8_t address;
    bool is_write;
    uint8_t type; /* first cycle frame type, 0 otherwise */
} wire_op_t;
static wire_op_t s_wire_log[4096];
static size_t s_wire_log_count;
static bool s_wire_log_enabled;
static int s_cycle_drop_go_channel = -1;
static int s_cycle_drop_plan_channel = -1;
static int s_cycle_drop_release_channel = -1;
static unsigned s_cycle_quiet_checks;
static unsigned s_cycle_node_count;
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
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
static void log_test_record(const char *line);
static void log_test_mux_write(void);
static bool log_test_read(sim_node_t *node, uint8_t *data, size_t length, esp_err_t *result);
static bool log_test_write(sim_node_t *node, const uint8_t *data, size_t length, esp_err_t *result);
static void log_test_probe(sim_node_t *node);
#endif

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
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
    log_test_record(line);
#endif
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

static uint64_t sim_cycle_now_ms(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time() / 1000U;
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
    const nrf_cs_cycle_runtime_ops_t cycle_ops = {.now_ms = sim_cycle_now_ms};
    nrf_cs_cycle_runtime_init(&node->cycle, &cycle_ops);
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

esp_err_t i2c_new_master_bus(
    const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus)
{
    assert(config != NULL && bus != NULL && !s_native_bus_live && !s_bus_live);
    assert(config->i2c_port == I2C_NUM_0);
    assert(config->sda_io_num == BOARD_SENSOR_I2C_SDA_GPIO);
    assert(config->scl_io_num == BOARD_SENSOR_I2C_SCL_GPIO);
    s_native_bus_live = true;
    *bus = &s_master_bus_token;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus == &s_master_bus_token && s_native_bus_live && !s_bus_live);
    s_native_bus_live = false;
    return ESP_OK;
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
    assert(s_native_bus_live && !s_bus_live);
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
    s_native_bus_live = false;
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
            s_devices[i].scl_wait_us = 0U;
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
    if (*device != NULL) {
        ((sim_device_t *)*device)->scl_wait_us = config->scl_wait_us;
    }
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
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
    log_test_mux_write();
#endif
    s_tca_control = data;
    ++s_tca_writes;
    return ESP_OK;
}

/*
 * A REMOTE answers an ordinary read with the 136 byte snapshot (its first 28
 * bytes are the STATUS). The fake builds it when the master asks for the part
 * beyond the first window, which is also when the injected snapshot faults apply.
 */
static void fake_compose_remote_snapshot(sim_node_t *node, uint8_t data[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH])
{
    nrf_upstream_status_t status;
    assert(nrf_bootstrap_make_status(&node->bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    const size_t data_len = NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH;
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
}

static void fake_window_write(sim_node_t *node, const uint8_t *data, size_t length)
{
    nrf_upstream_read_window_t window;
    uint8_t snapshot[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

    assert(nrf_upstream_protocol_decode_read_window(data, length, &window) == NRF_UPSTREAM_RESULT_OK);
    ++s_window_writes;
    if (node->window.length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH &&
        node->address == NRF_UPSTREAM_ADDRESS_REMOTE &&
        node->node_state.role == NRF_NODE_ROLE_REMOTE) {
        fake_compose_remote_snapshot(node, snapshot);
        nrf_read_window_target_record(&node->window, snapshot, sizeof(snapshot));
    }
    assert(nrf_read_window_target_arm(&node->window, window.offset)); /* the master asks only inside the response */
}

static void wire_log(uint8_t address, bool is_write, uint8_t type)
{
    if (s_wire_log_enabled && s_wire_log_count < sizeof(s_wire_log) / sizeof(s_wire_log[0])) {
        s_wire_log[s_wire_log_count++] = (wire_op_t){esp_timer_get_time(), address, is_write, type};
    }
}

/* Index of the undirected pair a<b among n nodes with ids 0..n-1, as the scheduler numbers them. */
static unsigned sim_pair_index(unsigned a, unsigned b, unsigned n)
{
    if (a > b) { const unsigned t = a; a = b; b = t; }
    return a * (2U * n - a - 1U) / 2U + (b - a - 1U);
}

static nrf_cs_measurement_t sim_cycle_measure(const nrf_cs_step_t *step)
{
    nrf_cs_measurement_t m = {.outcome = NRF_CS_OUTCOME_SUCCESS, .confidence = 900U};
    const unsigned pair = sim_pair_index(step->initiator, step->reflector, s_cycle_node_count);
    bool reflector_present = false;
    for (size_t i = 0U; i < MAX_NODES; ++i)
        reflector_present |= s_nodes[i].present && s_nodes[i].bootstrap.node_id == step->reflector;
    if (!reflector_present) {
        m.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
        m.confidence = 0U;
        return m;
    }
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
    if (s_cs_fail_pair_index == (int32_t)pair) {
        m.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
        m.confidence = 0U;
        return m;
    }
    m.distance_mm = s_cs_use_pair_distances ? s_cs_pair_distances[pair] : (int32_t)(1000U + pair);
#else
    m.distance_mm = (int32_t)(1000U + pair);
#endif
    return m;
}

/* The nodes' main loops: tick the cycle runtimes and finish each Bluetooth attempt at once. */
static void sim_cycle_tick_nodes(TickType_t ticks)
{
    (void)ticks;
    /* The quiet lease must cover every moment the coordinator waits. */
    assert(sensor_bus_quiet_active());
    ++s_cycle_quiet_checks;
    nrf_node_link_status_input_t status;
    assert(nrf_topology_service_get_node_status_snapshot(1U, &status) == ESP_OK);
    assert(status.assigned && status.status_valid && status.status_identity_matches);
    assert(status.polling_deferred);
    assert(status.topology_state == NRF_TOPOLOGY_NODE_ASSIGNED);
    for (int i = 0; i < MAX_NODES; ++i) {
        sim_node_t *node = &s_nodes[i];
        nrf_cs_step_t step;
        if (!node->present) continue;
        nrf_cs_cycle_runtime_tick(&node->cycle);
        if (nrf_cs_cycle_runtime_take_start(&node->cycle, &step)) {
            const nrf_cs_measurement_t m = sim_cycle_measure(&step);
            ++node->cycle_ble_attempts;
            nrf_cs_cycle_runtime_start_result(&node->cycle, &step, 0);
            assert(nrf_cs_cycle_runtime_step_done(&node->cycle, &step, &m) == 0);
        }
    }
}

/* Cycle frames the ESP32 writes; true when the frame was one of them. */
static bool sim_cycle_write(sim_node_t *node, const uint8_t *data, size_t length, esp_err_t *result)
{
    if (length < 4U || data[0] != 'G' || data[1] != 'S' || data[3] < NRF_CS_CYCLE_MESSAGE_PLAN ||
        data[3] > NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST) {
        return false;
    }
    *result = ESP_OK;
    node->cycle_reply_selected = false;
    switch (data[3]) {
    case NRF_CS_CYCLE_MESSAGE_PLAN: {
        nrf_cs_cycle_plan_t plan;
        ++node->cycle_plan_writes;
        if (s_cycle_drop_plan_channel == (int)node->channel) { *result = ESP_ERR_INVALID_STATE; break; }
        assert(nrf_cs_cycle_decode_plan(data, length, &plan) == 0);
        s_cycle_node_count = plan.node_count;
        assert(nrf_cs_cycle_runtime_accept_plan(&node->cycle, plan.self_node, &plan) == 0);
        break;
    }
    case NRF_CS_CYCLE_MESSAGE_PLAN_STEPS: {
        nrf_cs_cycle_plan_steps_t chunk;
        assert(nrf_cs_cycle_decode_plan_steps(data, length, &chunk) == 0);
        assert(nrf_cs_cycle_runtime_accept_steps(&node->cycle, &chunk) == 0);
        break;
    }
    case NRF_CS_CYCLE_MESSAGE_GO: {
        nrf_cs_cycle_go_t go;
        ++node->cycle_go_writes;
        if (s_cycle_drop_go_channel == (int)node->channel) { *result = ESP_ERR_INVALID_STATE; break; }
        assert(nrf_cs_cycle_decode_go(data, length, &go) == 0);
        assert(nrf_cs_cycle_runtime_go(&node->cycle, &go) == 0);
        break;
    }
    default: {
        nrf_cs_cycle_result_request_t request;
        nrf_cs_cycle_result_t page;
        assert(nrf_cs_cycle_decode_result_request(data, length, &request) == 0);
        if (request.page == NRF_CS_CYCLE_RESULT_RELEASE_PAGE) {
            if (s_cycle_drop_release_channel == (int)node->channel) { *result = ESP_ERR_INVALID_STATE; break; }
            nrf_cs_cycle_runtime_release(&node->cycle);
        } else if (nrf_cs_cycle_runtime_result_page(&node->cycle, request.request_id, request.page, &page) == 0) {
            assert(nrf_cs_cycle_encode_result(&page, node->cycle_reply) == 0);
            node->cycle_reply_selected = true;
        }
        break;
    }
    }
    return true;
}

esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    wire_log(d->address, false, 0U);
    if (d->address == BOARD_INA3221_I2C_ADDRESS) {
        assert(s_tca_control == 0U);
        memset(data, 0, data_len);
        return ESP_OK;
    }
    if (s_tca_control == 0U && d->address >= NRF_UPSTREAM_ADDRESS_DISCOVERY &&
        d->address <= NRF_UPSTREAM_ADDRESS_REMOTE) {
        ++s_direct_nrf_read_calls;
    }
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
        ++s_nack_read_calls;
        return ESP_ERR_INVALID_STATE; /* IDF 5.5 NACK */
    }
    /*
     * Wire contract: the master never reads an nRF target for more than one
     * window. The IDF driver splits longer reads into FIFO segments, and on the
     * 2026-10-26 capture every one of them arrived damaged.
     */
    assert(data_len > 0U && data_len <= NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES);
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
    esp_err_t log_result;
    if (log_test_read(node, data, data_len, &log_result)) return log_result;
#endif
    {
        const uint8_t *window_data = NULL;
        uint32_t window_length = 0U;
        if (nrf_read_window_target_serve(&node->window, &window_data, &window_length)) {
            assert(data_len <= window_length); /* the master never reads past what it selected */
            memcpy(data, window_data, data_len);
            ++s_window_reads;
            return ESP_OK;
        }
    }
    if (node->cycle_reply_selected) {
        assert(data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
        nrf_read_window_target_record(&node->window, node->cycle_reply, sizeof(node->cycle_reply));
        memcpy(data, node->cycle_reply, data_len);
        node->cycle_reply_selected = false;
        return ESP_OK;
    }
    if (nrf_cs_cycle_runtime_reply_active(&node->cycle) && data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        nrf_cs_cycle_status_t cycle_status;
        nrf_cs_cycle_runtime_status(&node->cycle, 0U, &cycle_status);
        assert(nrf_cs_cycle_encode_status(&cycle_status, data) == 0);
        nrf_read_window_target_record(&node->window, data, data_len);
        return ESP_OK;
    }
    if (node->cs_result_selected) {
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
        node->cs_result_selected = false;
        nrf_read_window_target_record(&node->window, frame, sizeof(frame));
        memcpy(data, frame, data_len);
        ++node->cs_status_reads;
        return ESP_OK;
    }
    nrf_upstream_status_t status;
    assert(nrf_bootstrap_make_status(&node->bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    if (data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        if (s_fail_next_main_status_read && d->address == NRF_UPSTREAM_ADDRESS_MAIN &&
            s_tca_control == 0U) {
            s_fail_next_main_status_read = false;
            return ESP_ERR_TIMEOUT;
        }
        if (s_log_reply_selected) {
            memset(data, 0, data_len);
            data[0] = 'G';
            data[1] = 'S';
            data[3] = NRF_LOG_MESSAGE_STATUS;
            ++node->status_reads;
            return ESP_OK;
        }
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
        nrf_read_window_target_record(&node->window, data, data_len);
        if (s_block_remote_scan_after_main_status && s_tca_control == 0U &&
            d->address == NRF_UPSTREAM_ADDRESS_MAIN) {
            s_fail_recursive_lock_attempts = 1000U;
        }
        if (node->delayed_assign_pending) {
            assert(nrf_bootstrap_process_assign(&node->bootstrap, &node->delayed_assign) ==
                   NRF_UPSTREAM_RESULT_OK);
            node->delayed_assign_pending = false;
            ++s_delayed_assignment_confirmations;
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
    wire_log(d->address, true, data_len > 3U && data[0] == 'G' && data[1] == 'S' ? data[3] : 0U);
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
    esp_err_t log_result;
    if (log_test_write(node, data, data_len, &log_result)) return log_result;
#endif
    {
        esp_err_t cycle_result;
        if (sim_cycle_write(node, data, data_len, &cycle_result)) return cycle_result;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH &&
        data[0] == 'G' && data[1] == 'S' &&
        data[3] == NRF_UPSTREAM_MESSAGE_READ_WINDOW) {
        fake_window_write(node, data, data_len);
        return ESP_OK;
    }
    if (data_len == NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH &&
        data[0] == 'G' && data[1] == 'S' &&
        data[3] == NRF_LOG_MESSAGE_STATUS_REQUEST) {
        s_log_reply_selected = true;
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
    if (data_len == NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH && data[3] == NRF_UPSTREAM_MESSAGE_SENSOR_SCAN) {
        nrf_upstream_sensor_refresh_t request;
        assert(nrf_upstream_protocol_decode_sensor_scan(data, data_len, &request) == NRF_UPSTREAM_RESULT_OK);
        node->sensor_scan_id = request.request_id;
        ++node->sensor_scans;
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
        if (s_fail_cs_write_type != 0U && data[3] == s_fail_cs_write_type &&
            node->channel == s_fail_cs_write_channel) {
            s_fail_cs_write_type = 0U;
            ++s_failed_cs_writes;
            return ESP_ERR_INVALID_STATE;
        }
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
        node->cs_result_selected = true;
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
    if (address_acks(address)) {
        if (address >= NRF_UPSTREAM_ADDRESS_DISCOVERY && address <= NRF_UPSTREAM_ADDRESS_REMOTE) {
            s_log_reply_selected = false;
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
            log_test_probe(node_at(address));
#endif
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

uint32_t sensor_bus_lines_boost_task_priority(uint32_t priority)
{
    (void)priority;
    return 0U;
}

void sensor_bus_lines_restore_task_priority(uint32_t previous_priority)
{
    (void)previous_priority;
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
static TaskHandle_t s_simulated_task;
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

struct test_semaphore { TaskHandle_t owner; unsigned depth; };
static struct test_semaphore s_mutex;
static struct test_semaphore s_bus_mutex;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return s_fail_mutex_create ? NULL : &s_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; assert(s_critical_depth == 0); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return s_fail_mutex_create ? NULL : &s_bus_mutex; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t)
{
    assert(s == &s_bus_mutex);
    (void)t;
    assert(s_critical_depth == 0);
    if (s_fail_recursive_lock_attempts > 0U) {
        --s_fail_recursive_lock_attempts;
        return pdFALSE;
    }
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (s->depth != 0U && s->owner != task) return pdFALSE;
    s->owner = task;
    ++s->depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    assert(s == &s_bus_mutex && s->depth != 0U && s->owner == xTaskGetCurrentTaskHandle());
    if (--s->depth == 0U) s->owner = NULL;
    return pdTRUE;
}

uint32_t esp_random(void) { s_random = s_random * 1103515245U + 12345U; return s_random; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 10000LL + (int64_t)s_fake_subtick_us; }
TickType_t xTaskGetTickCount(void) { return s_tick; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_simulated_task != NULL ? s_simulated_task : &s_task_token; }
const char *pcTaskGetName(TaskHandle_t task) { return task == &s_task_token ? "topology-test" : "ina-test"; }
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
    const TickType_t elapsed_ticks = ticks > 0U ? ticks : 1U;
    s_tick += elapsed_ticks;
    if (s_vtask_delay_hook != NULL) s_vtask_delay_hook(elapsed_ticks);
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t stack_depth, void *parameters,
    UBaseType_t priority, TaskHandle_t *created_task, BaseType_t core_id)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
#if defined(NRF_TOPOLOGY_SERVICE_TEST_LOG_COLLECTOR)
    assert(stack_depth == 8192U && priority == 2U);
#endif
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
    s_block_remote_scan_after_main_status = false;
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

static uint32_t s_requested_scan_id;
static void request_scan_from_handler(void)
{
    uint32_t ignored = 0U;
    assert(nrf_topology_service_request_sensor_scan(8U, &ignored) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_sensor_scan(2U, NULL) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_sensor_scan(4U, &ignored) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_sensor_scan(2U, &s_requested_scan_id) == ESP_OK);
    assert(s_requested_scan_id != 0U && s_nodes[1].sensor_scans == 0U);
    bool pending = false;
    esp_err_t dispatch_result = ESP_FAIL;
    assert(nrf_topology_service_get_sensor_scan_dispatch(s_requested_scan_id, &pending, &dispatch_result) == ESP_OK);
    assert(pending);
    assert(nrf_topology_service_request_sensor_scan(5U, &ignored) == ESP_ERR_INVALID_STATE);
}

static void assert_scan_dispatched(void)
{
    bool pending = true;
    esp_err_t result = ESP_FAIL;
    assert(nrf_topology_service_get_sensor_scan_dispatch(s_requested_scan_id, &pending, &result) == ESP_OK);
    assert(!pending && result == ESP_OK);
}

static void request_scan_then_disconnect(void)
{
    assert(nrf_topology_service_request_sensor_scan(2U, &s_requested_scan_id) == ESP_OK);
    s_nodes[1].present = false;
}

static void assert_scan_dispatch_failed(void)
{
    bool pending = true;
    esp_err_t result = ESP_OK;
    assert(nrf_topology_service_get_sensor_scan_dispatch(s_requested_scan_id, &pending, &result) == ESP_OK);
    assert(!pending && result != ESP_OK);
}

static void test_scan_worker_dispatches_only_to_selected_remote(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    node_boot(&s_nodes[2], 5U, 0x33U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_scan_from_handler);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_scan_dispatched);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_nodes[1].sensor_scans == 1U && s_nodes[1].sensor_scan_id == s_requested_scan_id);
    assert(s_nodes[2].sensor_scans == 0U && s_collisions == 0U && s_tca_control == 0U);
}

static void test_scan_dispatch_failure_is_reported(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_scan_then_disconnect);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_scan_dispatch_failed);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_nodes[1].sensor_scans == 0U && s_tca_control == 0U);
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

static void request_cs_with_write_fault(void)
{
    sensor_bus_owner_stats_t stats;
    assert(sensor_bus_get_owner_stats(&stats) == ESP_OK);
    s_cs_recovery_attempts_before = stats.recover_attempts;
    request_cs_cycle();
}

static void assert_cs_write_fault_was_classified(void)
{
    sensor_bus_owner_stats_t stats;
    assert(sensor_bus_get_owner_stats(&stats) == ESP_OK);
    assert(s_failed_cs_writes == 1U);
    assert(stats.recover_attempts > s_cs_recovery_attempts_before);
    capture();
    capture_cs();
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[3].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen_cs.complete && s_seen_cs.pair_count == 3U);
    for (size_t index = 0U; index < s_seen_cs.pair_count; ++index) {
        assert(s_seen_cs.pairs[index].state == NRF_CS_PAIR_VALID);
    }
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
        assert(s_seen_ranging_progress.pairs[pair].distance_mm == s_cs_pair_distances[pair]);
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
    for (size_t pair = 0U; pair < 6U; ++pair) {
        assert(s_seen_frozen_ranging_result.pairs[pair].node_a == s_seen_ranging_progress.pairs[pair].node_a);
        assert(s_seen_frozen_ranging_result.pairs[pair].node_b == s_seen_ranging_progress.pairs[pair].node_b);
        assert(s_seen_frozen_ranging_result.pairs[pair].distance_mm == s_cs_pair_distances[pair]);
    }
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
    assert(s_seen_ranging_progress.partial);
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_OK);
    assert(s_seen_frozen_ranging_result.available && !s_seen_frozen_ranging_result.success);
    nrf_topology_snapshot_t current;
    assert(nrf_topology_service_get_snapshot(&current) == ESP_OK);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &current, s_seen_frozen_ranging_result.timestamp_ms, 0U, &s_seen_frozen_ranging_result) == ESP_ERR_NOT_FOUND);
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
    /* Each of the 12 CS results is 44 bytes: the 28 byte read plus one window. */
    assert(s_window_writes >= 12U && s_window_writes == s_window_reads);
}

static int32_t s_seen_ranging_z_mm[4];
static size_t s_seen_ranging_z_count;

static void capture_ranging_z(void)
{
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_OK);
    assert(s_seen_frozen_ranging_result.available);
    s_seen_ranging_z_count = s_seen_frozen_ranging_result.geometry.node_count;
    for (size_t i = 0U; i < s_seen_ranging_z_count && i < 4U; ++i) {
        s_seen_ranging_z_mm[i] = s_seen_frozen_ranging_result.geometry.nodes[i].z_mm;
    }
}

/* The sensors read `reading_mm` against MAIN at this instant; the placement settled `settled_mm` (if `settled`). */
static void run_ranging_and_capture_z(bool settled, int32_t reading_mm, int32_t settled_mm)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    for (size_t index = 1U; index <= 3U; ++index) s_nodes[index].relative_z_mm = reading_mm;
    s_cs_use_pair_distances = true;
    const int32_t square_distances[6] = {1000, 1000, 1414, 1414, 1000, 1000};
    memcpy(s_cs_pair_distances, square_distances, sizeof(square_distances));
    nrf_topology_placement_heights_t heights = { .valid = settled, .generation = 91U };
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        heights.node_valid[channel] = true;
        heights.node_id[channel] = (nrf_cs_node_id_t)(channel + 1U);
        heights.height_rel_main_mm[channel] = settled_mm;
    }
    assert(nrf_topology_service_set_placement_heights(settled ? &heights : NULL) == ESP_OK);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_full_ranging_for_test);
    script_waits(2U, 10U);
    script_waits(2U, 10U);
    for (size_t pair = 1U; pair < 6U; ++pair) script_waits(4U, 10U);
    script(EV_ACTION, 0U, capture_ranging_z);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(nrf_topology_service_set_placement_heights(NULL) == ESP_OK);
}

static void test_ranging_geometry_takes_z_from_the_placement_heights(void)
{
    /* Without settled heights the geometry takes the reading of the instant: 300 mm for every remote. */
    run_ranging_and_capture_z(false, 300, 0);
    assert(s_seen_ranging_z_count == 4U);
    size_t remotes_at_reading = 0U;
    for (size_t i = 0U; i < s_seen_ranging_z_count; ++i) if (s_seen_ranging_z_mm[i] == 300) ++remotes_at_reading;
    assert(remotes_at_reading == 3U);

    /* With them it takes the settled height, whatever the sensors read now. */
    run_ranging_and_capture_z(true, 300, -120);
    assert(s_seen_ranging_z_count == 4U);
    size_t remotes_settled = 0U;
    for (size_t i = 0U; i < s_seen_ranging_z_count; ++i) {
        assert(s_seen_ranging_z_mm[i] != 300);
        if (s_seen_ranging_z_mm[i] == -120) ++remotes_settled;
    }
    assert(remotes_settled == 3U);
}

static void test_placement_heights_are_checked_against_the_reference_generation(void)
{
    nrf_topology_placement_heights_t heights = { .valid = true, .generation = 7U };
    heights.node_valid[0] = true;
    heights.node_id[0] = 4U;
    heights.height_rel_main_mm[0] = -85;
    heights.node_valid[1] = false;      /* a node the placement did not settle */
    heights.node_id[1] = 5U;
    heights.height_rel_main_mm[1] = 999;
    assert(nrf_topology_service_set_placement_heights(&heights) == ESP_OK);

    int32_t height_mm = 12345;
    assert(nrf_topology_service_get_placement_height_mm(4U, 7U, &height_mm) && height_mm == -85);
    assert(!nrf_topology_service_get_placement_height_mm(5U, 7U, &height_mm));    /* not settled */
    assert(!nrf_topology_service_get_placement_height_mm(6U, 7U, &height_mm));    /* unknown node */
    assert(!nrf_topology_service_get_placement_height_mm(4U, 8U, &height_mm));    /* another MAIN reference */
    assert(!nrf_topology_service_get_placement_height_mm(4U, 0U, &height_mm));    /* no generation */
    assert(!nrf_topology_service_get_placement_height_mm(4U, 7U, NULL));

    heights.valid = false;
    assert(nrf_topology_service_set_placement_heights(&heights) == ESP_OK);
    assert(!nrf_topology_service_get_placement_height_mm(4U, 7U, &height_mm));
    heights.valid = true;
    assert(nrf_topology_service_set_placement_heights(&heights) == ESP_OK);
    assert(nrf_topology_service_get_placement_height_mm(4U, 7U, &height_mm));
    assert(nrf_topology_service_set_placement_heights(NULL) == ESP_OK);
    assert(!nrf_topology_service_get_placement_height_mm(4U, 7U, &height_mm));
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

static const nrf_cs_policy_t k_autonomous_policy = {
    .pair_timeout_ms = 500U, .max_attempts = 1U, .min_confidence = 500U, .samples_per_pair = 1U, .full_rotation = true,
};

static void request_autonomous_ranging_for_test(void)
{
    s_wire_log_enabled = true;
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_OK);
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active && !s_seen_ranging_progress.complete);
    assert(s_seen_ranging_progress.pairs_total == 12U || s_seen_ranging_progress.pairs_total == 6U ||
           s_seen_ranging_progress.pairs_total == 2U);
    s_vtask_delay_hook = sim_cycle_tick_nodes;
}

static void clear_cycle_hook(void)
{
    s_vtask_delay_hook = NULL;
}

static uint64_t cycle_last_write_us(uint8_t type)
{
    int64_t last = 0;
    for (size_t i = 0U; i < s_wire_log_count; ++i) {
        if (s_wire_log[i].is_write && s_wire_log[i].type == type) last = s_wire_log[i].at_us;
    }
    return (uint64_t)last;
}

static void assert_autonomous_bus_silence(unsigned nodes)
{
    const unsigned executions = nodes * (nodes - 1U);
    const uint64_t window_us = (300U + 650U) * 1000ULL;
    const uint64_t go_end = cycle_last_write_us(NRF_CS_CYCLE_MESSAGE_GO);
    const uint64_t run_end = go_end + (uint64_t)executions * window_us;
    unsigned status_reads = 0U;
    int64_t previous = 0;
    assert(go_end != 0U);
    for (size_t i = 0U; i < s_wire_log_count; ++i) {
        const wire_op_t *op = &s_wire_log[i];
        if ((uint64_t)op->at_us <= go_end || (uint64_t)op->at_us >= run_end) continue;
        /* Nothing is written, nothing moves the mux, and only MAIN is read, at most every 5 s. */
        assert(!op->is_write);
        assert(op->address == NRF_UPSTREAM_ADDRESS_MAIN || op->address == TCA_ADDRESS);
        if (op->address == NRF_UPSTREAM_ADDRESS_MAIN) {
            if (previous != 0) assert(op->at_us - previous >= 5000 * 1000LL || op->at_us - previous < 50 * 1000LL);
            previous = op->at_us;
            ++status_reads;
        }
    }
    assert(status_reads >= 1U);
    assert(status_reads <= executions * 950U / 5000U + 2U);
}

static void assert_autonomous_success(void)
{
    assert_ranging_success_frozen();
    assert(!sensor_bus_quiet_active());
}

static void assert_autonomous_pair_failure(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "pair_failed:0-1") == 0);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    assert(s_seen_ranging_progress.partial);
    assert(nrf_topology_service_get_frozen_ranging_result(&s_seen_frozen_ranging_result) == ESP_OK);
    assert(s_seen_frozen_ranging_result.available && !s_seen_frozen_ranging_result.success);
    nrf_topology_snapshot_t current;
    assert(nrf_topology_service_get_snapshot(&current) == ESP_OK);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(
        &current, s_seen_frozen_ranging_result.timestamp_ms, 0U, &s_seen_frozen_ranging_result) == ESP_ERR_NOT_FOUND);
    assert(!sensor_bus_quiet_active());
}

static void assert_autonomous_silent_node(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete);
    /* The silent node still answers as a reflector, so its pairs keep the other direction only. */
    unsigned silent_pairs = 0U;
    for (size_t pair = 0U; pair < s_seen_ranging_progress.pair_count; ++pair) {
        const nrf_cs_pair_result_t *result = &s_seen_ranging_progress.pairs[pair];
        const bool involves_silent = result->node_a == 2U || result->node_b == 2U;
        assert(result->state == NRF_CS_PAIR_VALID);
        assert(result->single_direction == involves_silent);
        silent_pairs += involves_silent ? 1U : 0U;
    }
    assert(silent_pairs == 2U);
    assert(!sensor_bus_quiet_active());
}

static void assert_autonomous_plan_failure(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "plan_failed") == 0);
    assert(!sensor_bus_quiet_active());
}

static void manually_complete_failed_cycle(void)
{
    const unsigned writes = s_window_writes, reads = s_window_reads, mux = s_tca_writes;
    const uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT] = {2000U, 2000U};
    nrf_topology_ranging_progress_t progress;
    assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
    assert(progress.complete && !progress.success && !progress.partial && progress.pair_count == 3U);
    const int32_t distances[] = {1000, 1000, 1414};
    for (size_t i = 0U; i < 3U; ++i) {
        assert(nrf_topology_service_ranging_set_manual(progress.started_at_ms, progress.updated_at_ms,
            progress.pairs[i].node_a, progress.pairs[i].node_b, distances[i], limits) == ESP_OK);
        assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
        assert(progress.success == (i == 2U));
        assert(progress.partial == (i != 2U));
    }
    nrf_topology_snapshot_t topology;
    assert(nrf_topology_service_get_snapshot(&topology) == ESP_OK);
    nrf_topology_ranging_result_t frozen;
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(&topology,
        (uint64_t)(esp_timer_get_time() / 1000), 0U, &frozen) == ESP_OK);
    for (size_t i = 0U; i < frozen.geometry.node_count; ++i) assert(frozen.geometry.nodes[i].solved);
    topology.remotes[0].hardware_id[0] ^= 1U;
    assert(nrf_topology_service_test_publish_state(true, &topology, false, NULL) == ESP_OK);
    assert(nrf_topology_service_ranging_set_manual(progress.started_at_ms, progress.updated_at_ms,
        0U, 1U, 900, limits) == ESP_ERR_INVALID_STATE);
    assert(s_window_writes == writes && s_window_reads == reads && s_tca_writes == mux);
}

static unsigned total_cycle_ble_attempts(void)
{
    unsigned total = 0U;
    for (int i = 0; i < MAX_NODES; ++i) total += s_nodes[i].cycle_ble_attempts;
    return total;
}

static void test_autonomous_ranging_full_rotation(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    s_cs_use_pair_distances = true;
    const int32_t square_distances[6] = {1000, 1000, 1414, 1414, 1000, 1000};
    memcpy(s_cs_pair_distances, square_distances, sizeof(square_distances));

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL); /* the worker runs the whole cycle inside this wait */
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_success);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_cs_execute_count == 0U); /* no per-step CS_EXECUTE traffic at all */
    assert(total_cycle_ble_attempts() == 12U);
    assert(s_cycle_quiet_checks > 100U);
    for (int i = 0; i < 4; ++i) {
        assert(s_nodes[i].cycle_plan_writes == 1U && s_nodes[i].cycle_go_writes == 1U);
        assert(!nrf_cs_cycle_runtime_reply_active(&s_nodes[i].cycle)); /* collected and released */
    }
    assert_autonomous_bus_silence(4U);
}

static void test_autonomous_ranging_failures(void)
{
    /* One pair never measures: same failure the per-step path reports. */
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cs_fail_pair_index = 0;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_pair_failure);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_cs_execute_count == 0U);

    /* A node that never got GO stays silent: its executions time out, the others are kept. */
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cycle_drop_go_channel = 1;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_silent_node);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_nodes[2].cycle_ble_attempts == 0U && s_nodes[1].cycle_ble_attempts > 0U);

    /* A node that refuses its plan: nothing starts, the bus lease is given back. */
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cycle_drop_plan_channel = 1;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_plan_failure);
    script(EV_ACTION, 0U, manually_complete_failed_cycle);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(total_cycle_ble_attempts() == 0U);
    for (size_t i = 0U; i < s_wire_log_count; ++i) {
        assert(!(s_wire_log[i].is_write && s_wire_log[i].type == NRF_CS_CYCLE_MESSAGE_GO));
    }
}

/* ---- ranging cancel (BACK from RANGING must free the service) ---- */

static bool s_cancel_hook_fired;

static void cancel_after_go_hook(TickType_t ticks)
{
    sim_cycle_tick_nodes(ticks);
    if (!s_cancel_hook_fired && cycle_last_write_us(NRF_CS_CYCLE_MESSAGE_GO) != 0U) {
        s_cancel_hook_fired = true;
        assert(nrf_topology_service_ranging_cancel() == ESP_OK);
    }
}

static void request_autonomous_ranging_cancelled_mid_cycle(void)
{
    s_cancel_hook_fired = false;
    request_autonomous_ranging_for_test();
    s_vtask_delay_hook = cancel_after_go_hook;
}

static void assert_run_was_cancelled(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(!s_seen_ranging_progress.active);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "cancelled") == 0);
    assert(!sensor_bus_quiet_active());
}

static void assert_autonomous_run_cancelled_early(void)
{
    assert(s_cancel_hook_fired);
    assert_run_was_cancelled();
    assert(total_cycle_ble_attempts() < 6U); /* 3 nodes = 6 executions; the cycle did not run out */
    nrf_topology_snapshot_t current;
    nrf_topology_ranging_result_t reusable;
    assert(nrf_topology_service_get_snapshot(&current) == ESP_OK);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(&current, 0U, 0U, &reusable) == ESP_ERR_NOT_FOUND);
}

/* Cancelling stops the nodes too: none may keep ranging for the rest of the cycle. */
static void assert_cancel_freed_every_node(void)
{
    for (int i = 0; i < MAX_NODES; ++i) {
        if (!s_nodes[i].present) continue;
        assert(!nrf_cs_cycle_runtime_running(&s_nodes[i].cycle));
        assert(!nrf_cs_cycle_runtime_reply_active(&s_nodes[i].cycle));
    }
}

static void assert_new_run_not_aborted_by_stale_cancel(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "cancelled") != 0);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "aborted") != 0);
}

static void test_cancel_during_autonomous_cycle_frees_the_service(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_cancelled_mid_cycle);
    script(EV_WAIT_MS, 10U, NULL); /* the worker runs (and abandons) the cycle inside this wait */
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_run_cancelled_early);
    /* The service is free again at once, and the old cancel does not leak into the next run. */
    script(EV_ACTION, 0U, assert_cancel_freed_every_node);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_new_run_not_aborted_by_stale_cancel);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void request_then_cancel_before_worker_takes_it(void)
{
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_OK);
    assert(nrf_topology_service_ranging_cancel() == ESP_OK);
    assert_run_was_cancelled();
}

static void assert_cancelled_request_never_started_a_cycle(void)
{
    assert_run_was_cancelled();
    assert(total_cycle_ble_attempts() == 0U);
    for (size_t i = 0U; i < s_wire_log_count; ++i) {
        assert(!(s_wire_log[i].is_write && s_wire_log[i].type == NRF_CS_CYCLE_MESSAGE_PLAN));
    }
}

static void test_cancel_before_worker_takes_request(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    s_cs_use_pair_distances = true;
    const int32_t square_distances[6] = {1000, 1000, 1414, 1414, 1000, 1000};
    memcpy(s_cs_pair_distances, square_distances, sizeof(square_distances));
    s_wire_log_enabled = true;

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_then_cancel_before_worker_takes_it);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_cancelled_request_never_started_a_cycle);
    /* Service is free: a fresh run is accepted and completes normally. */
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_success);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void cancel_when_nothing_runs(void)
{
    assert(nrf_topology_service_ranging_cancel() == ESP_OK);
    assert(nrf_topology_service_ranging_cancel() == ESP_OK);
}

static void test_cancel_when_idle_is_noop_and_not_sticky(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    s_cs_use_pair_distances = true;
    const int32_t square_distances[6] = {1000, 1000, 1414, 1414, 1000, 1000};
    memcpy(s_cs_pair_distances, square_distances, sizeof(square_distances));

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, cancel_when_nothing_runs);
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_autonomous_success);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void cancel_stepwise_ranging(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.active && !s_seen_ranging_progress.complete);
    assert(nrf_topology_service_ranging_cancel() == ESP_OK);
}

static void assert_stepwise_run_cancelled(void)
{
    assert_run_was_cancelled();
    assert(s_cs_execute_count < 12U);
}

static void test_cancel_stepwise_ranging(void)
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
    script(EV_ACTION, 0U, cancel_stepwise_ranging);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_stepwise_run_cancelled);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void request_cancelled_cycle_that_loses_a_release(void)
{
    s_cycle_drop_release_channel = 1;
    request_autonomous_ranging_cancelled_mid_cycle();
}

static void assert_one_node_kept_ranging_and_heal_the_link(void)
{
    assert_run_was_cancelled();
    /* Channel 1's remote never heard the release: it is still ranging the abandoned cycle. */
    assert(nrf_cs_cycle_runtime_running(&s_nodes[2].cycle));
    assert(!nrf_cs_cycle_runtime_running(&s_nodes[0].cycle) && !nrf_cs_cycle_runtime_running(&s_nodes[1].cycle));
    s_cycle_drop_release_channel = -1;
}

static void assert_new_run_completed_on_every_node(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && s_seen_ranging_progress.success);
    assert(s_seen_ranging_progress.failure_reason[0] == '\0');
    for (int i = 0; i < 3; ++i) assert(!nrf_cs_cycle_runtime_reply_active(&s_nodes[i].cycle));
}

static void test_cycle_after_a_lost_release_clears_the_stale_node(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_cancelled_cycle_that_loses_a_release);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_one_node_kept_ranging_and_heal_the_link);
    /* The next cycle releases every node before planning, so the stale one takes part. */
    script(EV_ACTION, 0U, request_autonomous_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_new_run_completed_on_every_node);
    script(EV_STOP, 0U, NULL);
    run_service();
}

/* ---- shutdown: critical battery must not wait for a Channel Sounding cycle ---- */

/* Shutdown asks for the whole stop to take no more than this (device time). */
#define SHUTDOWN_CS_STOP_BUDGET_MS 2000U

static uint64_t s_disable_at_ms;

static void disable_after_go_hook(TickType_t ticks)
{
    sim_cycle_tick_nodes(ticks);
    if (!s_cancel_hook_fired && cycle_last_write_us(NRF_CS_CYCLE_MESSAGE_GO) != 0U) {
        s_cancel_hook_fired = true;
        s_disable_at_ms = (uint64_t)(esp_timer_get_time() / 1000);
        assert(nrf_topology_service_ranging_disable() == ESP_OK);
    }
}

static void request_autonomous_ranging_disabled_mid_cycle(void)
{
    s_cancel_hook_fired = false;
    request_autonomous_ranging_for_test();
    s_vtask_delay_hook = disable_after_go_hook;
}

static void assert_no_new_cs_cycle_can_start(void)
{
    const nrf_cs_policy_t policy = ranging_test_policy();
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_ranging_start(&policy, 1000U) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_cs_cycle(&policy) == ESP_ERR_INVALID_STATE);
}

static void assert_shutdown_stopped_the_cycle_quickly(void)
{
    assert(s_cancel_hook_fired);
    assert_run_was_cancelled();
    assert_cancel_freed_every_node();
    assert(s_seen_ranging_progress.updated_at_ms >= s_disable_at_ms);
    assert(s_seen_ranging_progress.updated_at_ms - s_disable_at_ms <= SHUTDOWN_CS_STOP_BUDGET_MS);
    assert_no_new_cs_cycle_can_start();
}

static void test_shutdown_stops_a_running_cycle_within_budget(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U); /* MAIN + 3 remotes: the longest realistic cycle */
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_autonomous_ranging_disabled_mid_cycle);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_shutdown_stopped_the_cycle_quickly);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void disable_while_idle(void)
{
    assert(nrf_topology_service_ranging_disable() == ESP_OK);
    assert(nrf_topology_service_ranging_disable() == ESP_OK);
    assert_no_new_cs_cycle_can_start();
}

static void request_then_disable_before_worker_takes_it(void)
{
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_OK);
    assert(nrf_topology_service_ranging_disable() == ESP_OK);
    assert_run_was_cancelled();
    assert_no_new_cs_cycle_can_start();
}

static void test_shutdown_refuses_new_cycles_even_when_idle(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, disable_while_idle);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_no_new_cs_cycle_can_start);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(total_cycle_ble_attempts() == 0U);

    /* A request that was queued but not taken yet is withdrawn as well. */
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(3U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_then_disable_before_worker_takes_it);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_cancelled_request_never_started_a_cycle);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void test_ranging_cancel(void)
{
    test_cancel_during_autonomous_cycle_frees_the_service();
    test_cancel_before_worker_takes_request();
    test_cancel_when_idle_is_noop_and_not_sticky();
    test_cancel_stepwise_ranging();
    test_cycle_after_a_lost_release_clears_the_stale_node();
    test_shutdown_stops_a_running_cycle_within_budget();
    test_shutdown_refuses_new_cycles_even_when_idle();
}

static void request_limited_ranging_for_test(void)
{
    const uint32_t missing[NRF_TOPOLOGY_REMOTE_COUNT] = {1000U};
    assert(nrf_topology_service_ranging_start_autonomous_limited(&k_autonomous_policy, missing) == ESP_ERR_INVALID_ARG);
    const uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT] = {1000U, 1001U};
    assert(nrf_topology_service_ranging_start_autonomous_limited(&k_autonomous_policy, limits) == ESP_OK);
    s_vtask_delay_hook = sim_cycle_tick_nodes;
}

static void assert_limited_ranging_for_test(void)
{
    assert(nrf_topology_service_ranging_get_progress(&s_seen_ranging_progress) == ESP_OK);
    assert(s_seen_ranging_progress.complete && !s_seen_ranging_progress.success);
    assert(strcmp(s_seen_ranging_progress.failure_reason, "cable_exceeded:0-2") == 0);
    assert(s_seen_ranging_progress.pairs[0].state == NRF_CS_PAIR_VALID);
    assert(s_seen_ranging_progress.pairs[0].distance_mm == 1000);
    assert(s_seen_ranging_progress.pairs[1].state == NRF_CS_PAIR_CABLE_EXCEEDED);
    assert(s_seen_ranging_progress.pairs[1].distance_mm == 1002);
    assert(s_seen_ranging_progress.pairs[2].state == NRF_CS_PAIR_VALID);
}

static void correct_limited_ranging_for_test(void)
{
    const uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT] = {1000U, 1001U};
    nrf_topology_ranging_progress_t before;
    assert(nrf_topology_service_ranging_get_progress(&before) == ESP_OK);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        0U, 2U, 1002, limits) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        0U, 2U, -1, limits) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        0U, 0U, 1000, limits) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        0U, 7U, 1000, limits) == ESP_ERR_NOT_FOUND);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms + 1U, before.updated_at_ms,
        0U, 2U, 1000, limits) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        2U, 0U, 1000, limits) == ESP_OK);
    nrf_topology_ranging_progress_t after;
    assert(nrf_topology_service_ranging_get_progress(&after) == ESP_OK);
    assert(after.success && !after.partial && after.complete);
    assert(after.updated_at_ms > before.updated_at_ms);
    assert(after.pairs[1].state == NRF_CS_PAIR_VALID && after.pairs[1].distance_mm == 1000);
    assert(after.pairs[1].automatic_distance_mm == 1002);
    assert((after.pairs[1].scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) != 0U);
    assert(after.geometry.node_count == 3U);
    for (size_t i = 0U; i < after.geometry.node_count; ++i) assert(after.geometry.nodes[i].solved);
    assert(nrf_topology_service_ranging_set_manual(before.started_at_ms, before.updated_at_ms,
        0U, 2U, 900, limits) == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_invalidate_ranging() == ESP_OK);
    assert(nrf_topology_service_ranging_set_manual(after.started_at_ms, after.updated_at_ms,
        0U, 2U, 900, limits) == ESP_ERR_INVALID_STATE);
}

static void test_autonomous_individual_cable_limits(void)
{
    reset_world();
    s_tca_present = true;
    prepare_ranging_nodes(2U);
    s_cs_use_pair_distances = true;
    s_cs_pair_distances[0] = 1000;
    s_cs_pair_distances[1] = 1002;
    s_cs_pair_distances[2] = 500;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_limited_ranging_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_limited_ranging_for_test);
    script(EV_ACTION, 0U, correct_limited_ranging_for_test);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void unplug_during_cycle(TickType_t ticks)
{
    s_nodes[1].present = false;
    sim_cycle_tick_nodes(ticks);
}

static void request_cycle_with_unplug(void)
{
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_OK);
    s_vtask_delay_hook = unplug_during_cycle;
}

static void verify_failed_cycle_and_rejoin(void)
{
    nrf_topology_ranging_progress_t progress;
    assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
    assert(progress.complete && !progress.active && !progress.success && progress.partial);
    assert(!sensor_bus_quiet_active());
    node_boot(&s_nodes[1], 0U, 0x22U);
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void test_unplug_during_cs_rejoins_after_quiet_lease(void)
{
    reset_world(); s_tca_present = true; prepare_ranging_nodes(2U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_cycle_with_unplug);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, verify_failed_cycle_and_rejoin);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[1].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[1].hardware_id[2] == 0x23U);
    assert(!sensor_bus_quiet_active() && s_tca_control == 0U);
}

static void request_inconsistent_mesh(void)
{
    assert(nrf_topology_service_ranging_start_autonomous(&k_autonomous_policy) == ESP_OK);
    s_vtask_delay_hook = sim_cycle_tick_nodes;
}

static void assert_inconsistent_mesh_and_correct(void)
{
    nrf_topology_ranging_progress_t progress;
    assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
    assert(progress.complete && progress.geometry.node_count == 5U);
    for (size_t i = 0U; i < progress.geometry.node_count; ++i) assert(progress.geometry.nodes[i].solved);
    assert(progress.geometry.inconsistent_pair_count > 0U);
    assert(progress.geometry.max_residual_mm > 750);
    assert(!progress.success && progress.partial);
    assert(strcmp(progress.failure_reason, "geometry_inconsistent") == 0);
    nrf_topology_snapshot_t topology;
    nrf_topology_ranging_result_t result;
    assert(nrf_topology_service_get_snapshot(&topology) == ESP_OK);
    assert(nrf_topology_service_get_reusable_frozen_ranging_result(&topology,
        (uint64_t)esp_timer_get_time() / 1000U, 0U, &result) == ESP_ERR_NOT_FOUND);
    const uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT] = {20000U, 20000U, 20000U, 20000U};
    assert(nrf_topology_service_ranging_set_manual(progress.started_at_ms, progress.updated_at_ms,
        3U, 4U, 3421, limits) == ESP_OK);
    assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
    assert(progress.success && !progress.partial && progress.failure_reason[0] == '\0');
}

static void test_inconsistent_solved_mesh_is_not_success(void)
{
    reset_world(); s_tca_present = true; prepare_ranging_nodes(4U);
    s_cs_use_pair_distances = true;
    /* Same 5-node mesh as the solver's gross-outlier regression; last pair is +3 m. */
    const int32_t distances[] = {5000, 6313, 3912, 3000, 4105, 6580, 3162, 5104, 3324, 6421};
    memcpy(s_cs_pair_distances, distances, sizeof(distances));
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_inconsistent_mesh);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_inconsistent_mesh_and_correct);
    script(EV_STOP, 0U, NULL);
    run_service();
}

static void test_autonomous_ranging_rejects_bad_requests(void)
{
    nrf_cs_policy_t policy = k_autonomous_policy;
    assert(nrf_topology_service_ranging_start_autonomous(NULL) == ESP_ERR_INVALID_ARG);
    policy.pair_timeout_ms = 70000U; /* does not fit the plan frame */
    assert(nrf_topology_service_ranging_start_autonomous(&policy) == ESP_ERR_INVALID_ARG);
    policy = k_autonomous_policy;
    policy.full_rotation = false;
    assert(nrf_topology_service_ranging_start_autonomous(&policy) == ESP_ERR_INVALID_ARG);
    policy = k_autonomous_policy;
    assert(nrf_topology_service_ranging_start_autonomous(&policy) == ESP_ERR_INVALID_STATE); /* no service */
}

static void request_noisy_limited_ranging(void)
{
    const uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT] = {1000U, 1000U, 1000U};
    assert(nrf_topology_service_ranging_start_autonomous_limited(&k_autonomous_policy, limits) == ESP_OK);
    s_vtask_delay_hook = sim_cycle_tick_nodes;
}

static void assert_noisy_geometry_respects_cables(void)
{
    nrf_topology_ranging_progress_t progress;
    assert(nrf_topology_service_ranging_get_progress(&progress) == ESP_OK);
    assert(progress.complete && !progress.success && progress.partial);
    for (size_t i = 0U; i < progress.pair_count; ++i) assert(progress.pairs[i].state == NRF_CS_PAIR_VALID);
    bool rejected = false;
    for (size_t i = 0U; i < progress.geometry.node_count; ++i) {
        const nrf_topology_ranging_node_t *node = &progress.geometry.nodes[i];
        const double radius2 = (double)node->x_mm * node->x_mm + (double)node->y_mm * node->y_mm + (double)node->z_mm * node->z_mm;
        if (node->node_id != 0U && radius2 > 1001.0 * 1001.0) {
            assert(!node->solved && node->uncertainty_mm == INT32_MAX);
            rejected = true;
        }
    }
    assert(rejected);
}

static void test_noisy_geometry_cannot_exceed_individual_cables(void)
{
    reset_world(); s_tca_present = true; prepare_ranging_nodes(3U);
    s_cs_use_pair_distances = true;
    const int32_t distances[] = {1000, 1000, 1000, 1900, 1900, 1900};
    memcpy(s_cs_pair_distances, distances, sizeof(distances));
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, request_noisy_limited_ranging);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, clear_cycle_hook);
    script(EV_ACTION, 0U, assert_noisy_geometry_respects_cables);
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
    assert(s_first_cs.pairs[0].attempts == 2U); /* same request retried, no extra radio attempt */
    assert(s_malformed_cs_results_seen == 1U);
    assert(s_seen_cs.complete && s_seen_cs.pair_count == NRF_CS_MAX_PAIRS);
    for (size_t pair = 0U; pair < s_seen_cs.pair_count; ++pair) {
        assert(s_seen_cs.pairs[pair].state == NRF_CS_PAIR_VALID);
        assert(s_seen_cs.pairs[pair].valid_samples == 1U);
    }
    assert(s_cs_execute_count == 12U); /* two-node executions plus all ten five-node pairs */
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
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
    main_barometer_fake_reset();
#endif
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
    s_window_writes = 0U;
    s_window_reads = 0U;
    s_direct_nrf_probe_calls = 0U;
    s_controller_reset_calls = 0U;
    s_fail_i2c_bus_create = false;
    s_fail_mutex_create = false;
    s_fail_task_create = false;
    s_fail_next_status_read = false;
    s_fail_next_main_status_read = false;
    s_timeout_failures_until_stuck = 0U;
    memset(s_status_response_frames, 0, sizeof(s_status_response_frames));
    s_status_response_frame_count = 0U;
    s_status_response_frame_index = 0U;
    s_arm_reselection_probe_after_status_read = false;
    s_reselection_probe_armed = false;
    s_fail_reselection_probe = false;
    s_log_reply_selected = false;
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
    s_fail_cs_write_type = 0U;
    s_fail_cs_write_channel = UPSTREAM_CHANNEL;
    s_failed_cs_writes = 0U;
    s_cs_recovery_attempts_before = 0U;
    s_in_height_calibration_handler = false;
    s_attempt_height_request_during_capture = false;
    s_height_request_during_capture_attempted = false;
    s_height_request_during_capture_result = ESP_ERR_INVALID_STATE;
    s_height_request_during_capture_generation = 0U;
    s_height_reference_writes = 0U;
    s_in_imu_calibration_handler = false;
    s_imu_calibration_writes = 0U;
    s_cs_hold_pending = false;
    s_wire_log_count = 0U;
    s_wire_log_enabled = false;
    s_cycle_drop_go_channel = -1;
    s_cycle_drop_plan_channel = -1;
    s_cycle_drop_release_channel = -1;
    s_cycle_quiet_checks = 0U;
    s_cycle_node_count = 0U;
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
    s_vtask_delay_hook = NULL;
    s_fail_recursive_lock_attempts = 0U;
    s_block_remote_scan_after_main_status = false;
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
        NRF_UPSTREAM_ADDRESS_DISCOVERY, &status, false);
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

static void test_status_reselects_before_reading_after_log(void)
{
    nrf_upstream_status_t status;
    prepare_direct_status_read_case();
    const unsigned reads_before = s_nodes[0].status_reads;
    const unsigned probes_before = s_probe_calls;
    assert(nrf_topology_service_test_read_status_cached_probe(
        NRF_UPSTREAM_ADDRESS_DISCOVERY, &status, true) == ESP_OK);
    assert(s_nodes[0].status_reads - reads_before == 1U);
    assert(s_probe_calls - probes_before == 1U);
    assert(!s_log_reply_selected);
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

static void test_cs_write_fault_preserves_two_remote_topology(void)
{
    static const struct {
        uint8_t type;
        uint8_t channel;
    } faults[] = {
        {NRF_CS_CONTROL_MESSAGE_EXECUTE, UPSTREAM_CHANNEL},
        {NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST, UPSTREAM_CHANNEL},
        {NRF_CS_CONTROL_MESSAGE_EXECUTE, 0U},
        {NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST, 0U},
    };
    for (size_t index = 0U; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        reset_world();
        s_tca_present = true;
        node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
        node_boot(&s_nodes[1], 0U, 0x22U);
        node_boot(&s_nodes[2], 3U, 0x33U);
        s_fail_cs_write_type = faults[index].type;
        s_fail_cs_write_channel = faults[index].channel;
        /* The shared recovery policy also rate-limits after the startup clear. */
        script(EV_WAIT_MS, 1010U, NULL);
        script(EV_ACTION, 0U, request_cs_with_write_fault);
        script_waits(32U, 10U);
        script(EV_ACTION, 0U, assert_cs_write_fault_was_classified);
        script(EV_STOP, 0U, NULL);
        run_service();
        assert(s_collisions == 0U);
        assert(s_tca_control == 0U);
    }
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
    /* A 136 byte snapshot is the 28 byte read plus four windows (28, 56, 84, 112). */
    assert(s_window_writes >= 4U && s_window_writes % 4U == 0U);
    assert(s_window_writes == s_window_reads);
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

static void block_remote_scan_after_main_status(void)
{
    s_block_remote_scan_after_main_status = true;
    assert(nrf_topology_service_request_refresh() == ESP_OK);
}

static void assert_fresh_main_despite_incomplete_remote_scan(void)
{
    s_block_remote_scan_after_main_status = false;
    clear_bus_busy_injection();
    nrf_topology_snapshot_t topology;
    nrf_node_link_status_input_t main_status;
    nrf_node_link_status_input_t remote_status;
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    assert(nrf_topology_service_get_snapshot(&topology) == ESP_OK);
    assert(topology.bus_busy && topology.stale);
    assert(nrf_topology_service_get_node_status_snapshot(0U, &main_status) == ESP_OK);
    assert(main_status.assigned && main_status.status_valid && main_status.status_identity_matches);
    assert(main_status.lifecycle == NRF_UPSTREAM_LIFECYCLE_READY);
    assert(now_ms - main_status.status_receipt_uptime_ms < NRF_NODE_LINK_STATUS_STALE_LIMIT_MS);
    assert(nrf_topology_service_get_node_status_snapshot(3U, &remote_status) == ESP_OK);
    assert(!remote_status.status_valid ||
        now_ms - remote_status.status_receipt_uptime_ms >= NRF_NODE_LINK_STATUS_STALE_LIMIT_MS);
}

static void test_main_status_survives_incomplete_remote_scan(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 2U, 0x22U);
    script(EV_WAIT_MS, 10U, NULL);
    for (unsigned period = 0U; period < 6U; ++period) {
        script(EV_ACTION, 0U, block_remote_scan_after_main_status);
        script(EV_WAIT_MS, NRF_NODE_LINK_STATUS_STALE_LIMIT_MS, NULL);
        script(EV_ACTION, 0U, assert_fresh_main_despite_incomplete_remote_scan);
    }
    script(EV_STOP, 0U, NULL);
    run_service();
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
static unsigned s_baro_polls_inside_bus_session;

/* The barometer runs between topology bus sessions, never inside one. */
static void baro_poll_checks_bus_is_free(void)
{
    sensor_bus_owner_stats_t stats = {0};
    assert(sensor_bus_get_owner_stats(&stats) == ESP_OK);
    if (stats.holder_task[0] != '\0') ++s_baro_polls_inside_bus_session;
}

static void assert_baro_polled_and_wait_bounded(void)
{
    assert(g_main_barometer_fake.poll_calls >= 1U);
    assert(s_last_notify_wait_ms >= 1U && s_last_notify_wait_ms <= 200U);
}

static unsigned s_baro_polls_before;

static void capture_baro_polls(void)
{
    s_baro_polls_before = g_main_barometer_fake.poll_calls;
}

static void assert_baro_polled_again_at_its_deadline(void)
{
    assert(g_main_barometer_fake.poll_calls >= s_baro_polls_before + 1U);
}

static void assert_idle_wait_ignores_unrequested_barometer(void)
{
    assert(g_main_barometer_fake.poll_calls >= 1U);
    /* Polling not requested: the task keeps its reconcile/log/sensor periods. */
    assert(s_last_notify_wait_ms > 200U);
}

static void test_main_barometer_is_polled_by_the_topology_task(void)
{
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    g_main_barometer_fake.polling_requested = true;
    g_main_barometer_fake.next_poll_delay_ms = 200U;
    g_main_barometer_fake.on_poll = baro_poll_checks_bus_is_free;
    g_main_barometer_fake.reading.bmp280_present = true;
    g_main_barometer_fake.reading.bmp280_valid = true;
    g_main_barometer_fake.reading.pressure_pa = 100650U;
    s_baro_polls_inside_bus_session = 0U;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_baro_polled_and_wait_bounded);
    script(EV_ACTION, 0U, capture_baro_polls);
    script(EV_WAIT_MS, 200U, NULL);
    script(EV_ACTION, 0U, assert_baro_polled_again_at_its_deadline);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(g_main_barometer_fake.init_calls == 1U);
    assert(g_main_barometer_fake.close_calls == 1U);
    assert(s_baro_polls_inside_bus_session == 0U);
    /* The reading reached the shared device status. */
    assert(g_main_barometer_fake.set_main_calls >= 1U);
    assert(g_main_barometer_fake.last_main_status.bmp280_valid);
    assert(g_main_barometer_fake.last_main_status.pressure_pa == 100650U);
}

static void test_unrequested_barometer_does_not_shorten_the_idle_wait(void)
{
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    g_main_barometer_fake.polling_requested = false;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, assert_idle_wait_ignores_unrequested_barometer);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(g_main_barometer_fake.close_calls == 1U);
}

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

/*
 * The nRF TWIS target is interrupt driven and stretches SCL until its response
 * is prepared; the controller default (about 2 ms) aborted MAIN reads. Only the
 * three nRF addresses get the longer allowance.
 */
static uint32_t s_scl_wait_by_address[128];
static bool s_scl_wait_seen[128];

static void capture_scl_waits(void)
{
    for (size_t i = 0U; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
        if (s_devices[i].live) {
            s_scl_wait_seen[s_devices[i].address] = true;
            s_scl_wait_by_address[s_devices[i].address] = s_devices[i].scl_wait_us;
        }
    }
}

static void test_nrf_target_devices_tolerate_scl_stretching(void)
{
    reset_world();
    memset(s_scl_wait_seen, 0, sizeof(s_scl_wait_seen));
    memset(s_scl_wait_by_address, 0, sizeof(s_scl_wait_by_address));
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);

    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_scl_waits);
    script(EV_STOP, 0U, NULL);
    run_service();

    static const uint8_t nrf_addresses[] = {
        NRF_UPSTREAM_ADDRESS_DISCOVERY, NRF_UPSTREAM_ADDRESS_MAIN, NRF_UPSTREAM_ADDRESS_REMOTE,
    };
    for (size_t i = 0U; i < sizeof(nrf_addresses); ++i) {
        assert(s_scl_wait_seen[nrf_addresses[i]]);
        assert(s_scl_wait_by_address[nrf_addresses[i]] == NRF_TOPOLOGY_TARGET_SCL_WAIT_US);
    }
    assert(NRF_TOPOLOGY_TARGET_SCL_WAIT_US > 2000U && NRF_TOPOLOGY_TARGET_SCL_WAIT_US <= 100000U);
    for (size_t address = 0U; address < sizeof(s_scl_wait_seen); ++address) {
        bool is_nrf = false;
        for (size_t i = 0U; i < sizeof(nrf_addresses); ++i) is_nrf = is_nrf || nrf_addresses[i] == address;
        if (s_scl_wait_seen[address] && !is_nrf) {
            assert(s_scl_wait_by_address[address] == 0U);
        }
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

static void release_stuck_scl_and_refresh(void)
{
    assert(s_controller_reset_calls == 0U);
    s_scl_stuck = false;
    request_topology_refresh_for_test();
}

static nrf_topology_snapshot_t s_main_timeout_snapshot;
static nrf_topology_snapshot_t s_main_timeout_busy_snapshot;
static nrf_remote_sensor_cache_t s_main_timeout_cache[2];
static esp_err_t s_main_timeout_height_request;
static esp_err_t s_main_timeout_imu_request;
static esp_err_t s_main_timeout_cs_request;
static esp_err_t s_main_timeout_ranging_request;
static esp_err_t s_main_timeout_busy_ranging_request;

static void request_main_timeout_reconcile(void)
{
    s_fail_next_main_status_read = true;
    request_topology_refresh_for_test();
}

static void capture_main_timeout_snapshot(void)
{
    uint8_t mask = 0U;
    uint16_t generation = 0U;
    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 100U, .max_attempts = 1U,
        .min_confidence = 500U, .full_rotation = true,
    };
    capture();
    s_main_timeout_snapshot = s_seen;
    s_main_timeout_cache[0] = s_seen_cache[0];
    s_main_timeout_cache[1] = s_seen_cache[3];
    s_main_timeout_height_request =
        nrf_topology_service_request_height_reference_capture(&mask, &generation);
    s_main_timeout_imu_request = nrf_topology_service_request_imu_calibration(1U, 1U);
    s_main_timeout_cs_request = nrf_topology_service_request_cs_cycle(&policy);
    s_main_timeout_ranging_request = nrf_topology_service_ranging_start(&policy, 1000U);
}

static void capture_main_timeout_busy_snapshot(void)
{
    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 100U, .max_attempts = 1U,
        .min_confidence = 500U, .full_rotation = true,
    };
    capture();
    s_main_timeout_busy_snapshot = s_seen;
    s_main_timeout_busy_ranging_request = nrf_topology_service_ranging_start(&policy, 1000U);
}

static void test_main_timeout_preserves_stale_topology_until_recovery(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 0U, 0x22U);
    node_boot(&s_nodes[2], 3U, 0x33U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_initial_topology);
    script(EV_ACTION, 0U, request_main_timeout_reconcile);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_main_timeout_snapshot);
    script(EV_ACTION, 0U, request_refresh_while_bus_busy);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_main_timeout_busy_snapshot);
    script(EV_ACTION, 0U, clear_bus_busy_injection);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(!s_fail_next_main_status_read);
    assert(assigned_node_count(&s_initial_topology) == 3U);
    assert(s_main_timeout_snapshot.stale && !s_main_timeout_snapshot.bus_busy);
    assert(s_main_timeout_snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(s_main_timeout_snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_main_timeout_snapshot.remotes[3].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    /* Superseded 2026-10-31: a MAIN-only fault leaves the REMOTE caches current and does not refuse
     * REMOTE-only work. Bench: calibration was refused / its MAIN row lost whenever MAIN failed one read.
     * CS and ranging include MAIN as a node and are still refused. */
    assert(s_main_timeout_cache[0].identity_valid && s_main_timeout_cache[0].topology_current);
    assert(s_main_timeout_cache[1].identity_valid && s_main_timeout_cache[1].topology_current);
    assert(s_main_timeout_height_request == ESP_OK);
    assert(s_main_timeout_imu_request == ESP_OK);
    assert(s_main_timeout_cs_request == ESP_ERR_INVALID_STATE);
    assert(s_main_timeout_ranging_request == ESP_ERR_INVALID_STATE);
    assert(s_main_timeout_busy_ranging_request == ESP_ERR_INVALID_STATE);
    assert(s_main_timeout_busy_snapshot.stale && s_main_timeout_busy_snapshot.bus_busy);
    assert(assigned_node_count(&s_main_timeout_busy_snapshot) == 3U);
    assert(!s_seen.stale && !s_seen.bus_busy && assigned_node_count(&s_seen) == 3U);
    assert(s_seen_cache[0].topology_current && s_seen_cache[3].topology_current);
    assert(s_collisions == 0U && s_tca_control == 0U);
}

static void test_main_timeout_does_not_extend_stale_topology_lifetime(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 0U, 0x22U);
    node_boot(&s_nodes[2], 3U, 0x33U);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_initial_topology);
    for (unsigned i = 0U; i < 5U; ++i) {
        script(EV_ACTION, 0U, request_main_timeout_reconcile);
        script(EV_WAIT_MS, NRF_TOPOLOGY_RECONCILE_PERIOD_MS, NULL);
    }
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(assigned_node_count(&s_initial_topology) == 3U);
    assert(s_seen.stale && s_seen.age_ms >= NRF_TOPOLOGY_STALE_LIMIT_MS);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(s_seen.remotes[3].state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(s_collisions == 0U && s_tca_control == 0U);
}

static void test_main_timeout_without_a_previous_topology(void)
{
    reset_world();
    s_tca_present = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    nrf_upstream_assign_t assign = {
        .request_id = 1U,
        .hardware_id_length = sizeof(s_nodes[0].hardware_id),
        .role = NRF_UPSTREAM_ROLE_MAIN,
        .node_id = NRF_TOPOLOGY_MAIN_NODE_ID,
        .mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE,
    };
    memcpy(assign.hardware_id, s_nodes[0].hardware_id, sizeof(s_nodes[0].hardware_id));
    assert(nrf_bootstrap_process_assign(&s_nodes[0].bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    s_fail_next_main_status_read = true;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(!s_fail_next_main_status_read);
    assert(s_seen.stale && !s_seen.bus_busy);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(assigned_node_count(&s_seen) == 0U);
    assert(s_collisions == 0U && s_tca_control == 0U);
}

static void test_mux_discovery_timeouts_escalate_and_rejoin(void)
{
    reset_world();
    s_tick = 500000U; /* Beyond recovery backoff from earlier scenarios. */
    s_tca_present = true;
    s_scl_stuck = true;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    node_boot(&s_nodes[1], 0U, 0x22U);
    sensor_bus_recovery_status_t before;
    assert(sensor_bus_get_recovery_status(&before) == ESP_OK);
    s_timeout_recovery_start = before.attempts + 1U; /* Boot-time line check. */

    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_no_line_recovery_yet);
    script(EV_ACTION, 0U, request_topology_refresh_for_test);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_ACTION, 0U, assert_line_recovery_attempted);
    script(EV_ACTION, 0U, release_stuck_scl_and_refresh);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture);
    script(EV_STOP, 0U, NULL);
    run_service();

    assert(s_seen.tca_available && !s_seen.tca_fault);
    assert(s_seen.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(s_seen.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    for (size_t channel = 1U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assert(s_seen.remotes[channel].state == NRF_TOPOLOGY_NODE_ABSENT);
    }
    assert(s_collisions == 0U);
}

int main(void)
{
    test_api_guards_when_stopped();
    test_lifecycle_failures_mid_reconcile_and_repeated_restart();
    test_stale_log_response_reselects_status();
    test_status_reselects_before_reading_after_log();
    test_assignment_confirmation_race_preserves_node_mapping();
    test_malformed_snapshot_marks_cached_sample_stale();
    test_unplug_and_replug_between_reconciles();
    test_full_topology_is_assigned_one_hot();
    test_mux_main_address_conflict_blocks_direct_nrf_io();
    test_cs_write_fault_preserves_two_remote_topology();
    test_remote_snapshot_v1_and_v2_cache_read_path();
    test_height_calibration_worker_queue_and_generation_wrap();
    test_height_calibration_without_remotes();
    test_imu_calibration_worker_dispatch_and_v3_cache();
    test_scan_worker_dispatches_only_to_selected_remote();
    test_scan_dispatch_failure_is_reported();
    test_without_tca_only_main_is_managed();
    test_nrf_target_devices_tolerate_scl_stretching();
    test_stack_phase_tracking_and_cs_cycle_path();
    test_error_recovery_stack_hwm_and_long_error_name();
    test_persistent_timeouts_escalate_after_three_windows();
    test_reconcile_deferred_during_recording_start_backoff();
    test_topology_and_sensor_refresh_holds_stay_within_budget();
    test_sensor_refresh_requests_survive_busy_reconcile();
    test_busy_window_preserves_last_good_snapshot();
    test_main_status_survives_incomplete_remote_scan();
    test_snapshot_becomes_unknown_after_stale_limit();
    test_named_reconcile_and_sensor_periods_on_fake_clock();
#if defined(NRF_TOPOLOGY_SERVICE_TEST_RANGING)
    test_main_barometer_is_polled_by_the_topology_task();
    test_unrequested_barometer_does_not_shorten_the_idle_wait();
    test_cs_pump_period_on_fake_clock();
    test_ranging_full_rotation_progress_and_frozen_result();
    test_ranging_geometry_takes_z_from_the_placement_heights();
    test_placement_heights_are_checked_against_the_reference_generation();
    test_ranging_failure_reasons();
    test_ranging_default_timeout_is_30_seconds();
    test_ranging_requires_assigned_main();
    test_autonomous_ranging_full_rotation();
    test_autonomous_ranging_failures();
    test_ranging_cancel();
    test_autonomous_individual_cable_limits();
    test_noisy_geometry_cannot_exceed_individual_cables();
    test_unplug_during_cs_rejoins_after_quiet_lease();
    test_inconsistent_solved_mesh_is_not_success();
    test_autonomous_ranging_rejects_bad_requests();
#endif
    test_mux_discovery_timeouts_escalate_and_rejoin();
    test_main_timeout_preserves_stale_topology_until_recovery();
    test_main_timeout_does_not_extend_stale_topology_lifetime();
    test_main_timeout_without_a_previous_topology();
    return 0;
}
