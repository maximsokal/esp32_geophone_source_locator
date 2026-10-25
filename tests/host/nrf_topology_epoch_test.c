#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nrf_topology.h"

typedef struct {
    int selected;
    nrf_upstream_status_t main_status;
    nrf_upstream_status_t remote_status;
} fake_bus_t;

static void make_status(nrf_upstream_status_t *s, uint8_t role, uint8_t channel, uint8_t hw, uint32_t request_id)
{
    memset(s, 0, sizeof(*s));
    s->request_id = request_id;
    s->hardware_id_length = 4U;
    s->hardware_id[0] = hw;
    s->hardware_id[1] = (uint8_t)(hw + 1U);
    s->hardware_id[2] = (uint8_t)(hw + 2U);
    s->hardware_id[3] = (uint8_t)(hw + 3U);
    s->role = role;
    s->node_id = role == NRF_UPSTREAM_ROLE_MAIN ? 0U : (uint8_t)(channel + 1U);
    s->mux_channel = role == NRF_UPSTREAM_ROLE_MAIN ? NRF_UPSTREAM_MUX_CHANNEL_NONE : channel;
    s->target_address = role == NRF_UPSTREAM_ROLE_MAIN ? NRF_UPSTREAM_ADDRESS_MAIN : NRF_UPSTREAM_ADDRESS_REMOTE;
    s->lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    s->result = NRF_UPSTREAM_RESULT_OK;
    s->flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
}

static esp_err_t close_mux(void *context) { ((fake_bus_t*)context)->selected = -1; return ESP_OK; }
static esp_err_t select_mux(void *context, uint8_t channel) { ((fake_bus_t*)context)->selected = channel; return ESP_OK; }
static esp_err_t read_status(void *context, uint8_t address, nrf_upstream_status_t *status)
{
    fake_bus_t *b = context;
    if (address == NRF_UPSTREAM_ADDRESS_DISCOVERY) return ESP_ERR_NOT_FOUND;
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) { *status = b->main_status; return ESP_OK; }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE) {
        if (b->selected == 2) { *status = b->remote_status; return ESP_OK; }
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_ERR_INVALID_ARG;
}
static esp_err_t write_assign(void *context, uint8_t address, const nrf_upstream_assign_t *assign) { (void)context; (void)address; (void)assign; return ESP_FAIL; }
static void sleep_ms(void *context, uint32_t ms) { (void)context; (void)ms; }
static uint64_t now_ms(void *context) { (void)context; return 0U; }

int main(void)
{
    fake_bus_t bus = {.selected = -1};
    make_status(&bus.main_status, NRF_UPSTREAM_ROLE_MAIN, 0U, 0x10U, 0x11112222U);
    make_status(&bus.remote_status, NRF_UPSTREAM_ROLE_REMOTE, 2U, 0x30U, 0x33334444U);
    nrf_topology_controller_t controller;
    const nrf_topology_config_t cfg = {500U, 10U, 1U};
    nrf_topology_snapshot_t snapshot;
    const nrf_topology_io_t io = {
        .context = &bus,
        .tca_available = true,
        .tca_fault = false,
        .mux_close = close_mux,
        .mux_select = select_mux,
        .read_status = read_status,
        .write_assign = write_assign,
        .now_ms = now_ms,
        .sleep_ms = sleep_ms,
    };
    assert(nrf_topology_controller_init(&controller, &cfg) == ESP_OK);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.main.assignment_request_id == 0x11112222U);
    assert(snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[2].assignment_request_id == 0x33334444U);
    return 0;
}
