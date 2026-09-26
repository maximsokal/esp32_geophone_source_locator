#include <assert.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "nrf_bootstrap.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_node_state.h"
#include "nrf_topology_service.h"
#include "nrf_upstream_protocol.h"
#include "sensor_bus.h"
#include "sensor_bus_lines.h"

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
    unsigned height_reference_captures;
    uint16_t captured_generation[8];
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
static sim_device_t s_devices[64];
static int s_bus_token;
static bool s_bus_live;
static uint32_t s_tick;
static unsigned s_collisions;
static bool s_fail_i2c_bus_create;
static bool s_fail_mutex_create;
static bool s_fail_task_create;
static bool s_fail_next_status_read;
static bool s_scl_stuck;
static bool s_stop_on_status_read;
static bool s_reconcile_stop_waiting;
static jmp_buf s_reconcile_stop_return;
static unsigned s_malformed_cs_results_remaining;
static unsigned s_malformed_cs_results_seen;
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
static bool s_in_height_calibration_handler;
static unsigned s_height_reference_writes;
static nrf_upstream_height_reference_t s_height_reference_requests[32];

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
    node->snapshot_version = 2U;
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
            }
            found = &s_nodes[i];
        }
    }
    /* Two XIAOs driving one address corrupt every frame on real wires. */
    assert(s_collisions == 0U);
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

esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    if (d->address == TCA_ADDRESS && s_tca_present) {
        *data = s_tca_control;
        return ESP_OK;
    }
    if (d->address >= 0x70U && d->address <= 0x77U) {
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
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
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
            return ESP_ERR_TIMEOUT;
        }
        ++node->status_reads;
        assert(nrf_upstream_protocol_encode_status(&status, data) == NRF_UPSTREAM_RESULT_OK);
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
        return ESP_OK;
    }
    assert(false);
    return ESP_FAIL;
}

esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data)
{
    sim_device_t *d = device_of(device);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    sim_node_t *node = node_at(d->address);
    if (node == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH) {
        nrf_upstream_assign_t assign;
        assert(nrf_upstream_protocol_decode_assign(data, data_len, &assign) == NRF_UPSTREAM_RESULT_OK);
        (void)nrf_bootstrap_process_assign(&node->bootstrap, &assign);
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
        assert(node->height_reference_captures <
               sizeof(node->captured_generation) / sizeof(node->captured_generation[0]));
        node->captured_generation[node->height_reference_captures++] = request.generation;
        assert(s_height_reference_writes <
               sizeof(s_height_reference_requests) / sizeof(s_height_reference_requests[0]));
        s_height_reference_requests[s_height_reference_writes++] = request;
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
esp_err_t sensor_bus_lines_bitbang_io(i2c_line_io_t *io)
{
    (void)io;
    return ESP_ERR_INVALID_STATE; /* recorded as a line fault; bus still created */
}

void sensor_bus_lines_read(bool *sda_high, bool *scl_high)
{
    *sda_high = true;
    *scl_high = !s_scl_stuck;
}

esp_err_t sensor_bus_lines_controller_reset(i2c_bus_handle_t bus)
{
    (void)bus;
    assert(false); /* lines are never stuck in these scenarios */
    return ESP_FAIL;
}

esp_err_t sensor_bus_lines_probe(i2c_bus_handle_t bus, uint8_t address)
{
    assert(bus == &s_bus_token);
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

uint32_t esp_random(void) { s_random = s_random * 1103515245U + 12345U; return s_random; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 10000LL; }
TickType_t xTaskGetTickCount(void) { return s_tick; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task_token; }
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
    (void)ticks_to_wait;
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

static void assert_height_capture_counts_and_generation_65535(void)
{
    assert(s_height_requested_mask == ((uint8_t)(1U << 2U) | (uint8_t)(1U << 5U)));
    assert(s_height_requested_generation == UINT16_MAX);
    assert(s_height_reference_writes == 2U);
    assert(s_nodes[1].height_reference_captures == 1U);
    assert(s_nodes[2].height_reference_captures == 1U);
    assert(s_nodes[1].captured_generation[0] == UINT16_MAX);
    assert(s_nodes[2].captured_generation[0] == UINT16_MAX);
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

static void test_error_recovery_stack_hwm_and_long_error_name(void)
{
    reset_world();
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    s_fail_next_status_read = true;
    s_scl_stuck = true;
    memset(s_long_error_name, 'E', sizeof(s_long_error_name) - 1U);
    s_long_error_name[sizeof(s_long_error_name) - 1U] = '\0';
    s_stack_hwm_sequence[0] = 700U; /* error recovery executes before reconcile returns */
    s_stack_hwm_sequence[1] = 1800U;
    s_stack_hwm_sequence_count = 2U;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_STOP, 0U, NULL);
    run_service();

    nrf_topology_service_stack_stats_t stats;
    assert(nrf_topology_service_get_stack_stats(&stats) == ESP_OK);
    assert(stats.phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_ERROR_RECOVERY] == 700U);
    assert(s_stack_hwm_warning_logs >= 1U);
    assert(s_long_error_was_logged);
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
    s_tca_present = false;
    s_tca_control = 0U;
    s_tca_writes = 0U;
    s_collisions = 0U;
    s_fail_i2c_bus_create = false;
    s_fail_mutex_create = false;
    s_fail_task_create = false;
    s_fail_next_status_read = false;
    s_scl_stuck = false;
    s_stop_on_status_read = false;
    s_reconcile_stop_waiting = false;
    s_malformed_cs_results_remaining = 0U;
    s_malformed_cs_results_seen = 0U;
    s_cs_execute_count = 0U;
    s_in_height_calibration_handler = false;
    s_height_reference_writes = 0U;
    s_height_request_result = ESP_ERR_INVALID_STATE;
    s_height_requested_mask = 0U;
    s_stack_hwm_default = 2048U;
    s_stack_hwm_sequence_count = 0U;
    s_stack_hwm_sequence_pos = 0U;
    s_stack_hwm_warning_logs = 0U;
    s_long_error_name[0] = '\0';
    s_long_error_was_logged = false;
    s_script_len = 0U;
    s_script_pos = 0U;
    s_tick = 1000U;
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
    script(EV_ACTION, 0U, unplug_channel5);
    script(EV_WAIT_MS, 5000U, NULL);    /* periodic reconcile */
    script(EV_ACTION, 0U, capture);
    script(EV_ACTION, 0U, replug_channel5);
    script(EV_WAIT_MS, 5000U, NULL);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_STOP, 0U, NULL);

    assert(sensor_bus_lock_init() == ESP_OK);
    assert(nrf_topology_service_start() == ESP_OK);
    nrf_topology_service_stop();

    /* After the unplug the channel read as absent, not as a fault. */
    assert(s_seen.remotes[5].state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(s_seen.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
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
    uint8_t height_mask = UINT8_MAX;
    uint16_t height_generation = UINT16_MAX;
    assert(nrf_topology_service_request_refresh() == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_sensor_refresh(NRF_TOPOLOGY_REMOTE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_request_height_reference_capture(
        &height_mask, &height_generation) == ESP_ERR_INVALID_STATE);
    assert(height_mask == 0U);
    assert(height_generation == 0U);
    assert(nrf_topology_service_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    (void)snapshot;
    /* stop() on a stopped service returns immediately. */
    nrf_topology_service_stop();
}

int main(void)
{
    test_api_guards_when_stopped();
    test_lifecycle_failures_mid_reconcile_and_repeated_restart();
    test_full_topology_is_assigned_one_hot();
    test_remote_snapshot_v1_and_v2_cache_read_path();
    test_height_calibration_worker_queue_and_generation_wrap();
    test_height_calibration_without_remotes();
    test_without_tca_only_main_is_managed();
    test_unplug_and_replug_between_reconciles();
    test_stack_phase_tracking_and_cs_cycle_path();
    test_error_recovery_stack_hwm_and_long_error_name();
    return 0;
}
