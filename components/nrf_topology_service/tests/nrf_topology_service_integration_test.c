#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "nrf_bootstrap.h"
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
    unsigned status_reads;
    unsigned sensor_refreshes;
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
static unsigned s_collisions;

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
    nrf_upstream_status_t status;
    assert(nrf_bootstrap_make_status(&node->bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    if (data_len == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        ++node->status_reads;
        assert(nrf_upstream_protocol_encode_status(&status, data) == NRF_UPSTREAM_RESULT_OK);
        return ESP_OK;
    }
    if (data_len == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH) {
        nrf_upstream_sensor_snapshot_t sensor;
        memset(&sensor, 0, sizeof(sensor));
        sensor.request_id = node->sensor_request_id;
        sensor.result = NRF_UPSTREAM_SENSOR_RESULT_NO_DATA;
        assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, data) == NRF_UPSTREAM_RESULT_OK);
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
    *scl_high = true;
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

static event_t s_script[32];
static size_t s_script_len;
static size_t s_script_pos;
static uint32_t s_tick;
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

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; assert(s_critical_depth == 0); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }

uint32_t esp_random(void) { s_random = s_random * 1103515245U + 12345U; return s_random; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 10000LL; }
TickType_t xTaskGetTickCount(void) { return s_tick; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task_token; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 2048U; }
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task == &s_task_token); return pdPASS; }

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    assert(s_in_worker);
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_critical_depth == 0);
    if (s_stop_waiting) {
        s_stop_waiting = false;
        longjmp(s_stop_return, 1);
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
    *created_task = &s_task_token;
    s_in_worker = true;
    entry(parameters);
    s_in_worker = false;
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

static void reset_world(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    memset(s_devices, 0, sizeof(s_devices));
    s_tca_present = false;
    s_tca_control = 0U;
    s_tca_writes = 0U;
    s_collisions = 0U;
    s_script_len = 0U;
    s_script_pos = 0U;
    s_tick = 1000U;
    s_critical_depth = 0;
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
    assert(nrf_topology_service_request_refresh() == ESP_ERR_INVALID_STATE);
    assert(nrf_topology_service_request_sensor_refresh(NRF_TOPOLOGY_REMOTE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_service_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    (void)snapshot;
    /* stop() on a stopped service returns immediately. */
    nrf_topology_service_stop();
}

int main(void)
{
    test_api_guards_when_stopped();
    test_full_topology_is_assigned_one_hot();
    test_without_tca_only_main_is_managed();
    test_unplug_and_replug_between_reconciles();
    return 0;
}
