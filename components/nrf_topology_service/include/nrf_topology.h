#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_upstream_protocol.h"

#define NRF_TOPOLOGY_REMOTE_COUNT 8U
#define NRF_TOPOLOGY_MAIN_NODE_ID 0U
#define NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS 500U
/*
 * An automatic Channel Sounding distance is quantized by one bin of the FFT-512 IFFT, c / (2 * 512 * 1 MHz) = 292.8 mm
 * (the bench cycle of 2026-11-05 shows only multiples of 292.75 mm). A distance that exceeds a cable by less than one
 * bin therefore does not show that the node is beyond it. Manual distances are exact and get no allowance.
 */
#define NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM 293U
#define NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS 10U

/*
 * Channel quarantine.
 *
 * Selecting a TCA9548A channel connects that socket's wires to the shared bus. A socket whose wiring holds a line
 * (board log 2026-11-10: the empty socket on channel 1) makes the select time out; the mux then cannot be told to
 * close, and once it has latched the channel the whole bus, with INA3221 and the MAIN barometer on it, stays dead
 * until the mux is power-cycled. Discovery used to select every channel on every pass, so such a socket was hit
 * about every 8 s and the sixth hit was the fatal one.
 *
 * A channel whose select failed is therefore left alone for a while: 60 s after the failure that triggers the
 * quarantine, doubling with each further failure up to one hour. A channel that carried a confirmed node is given
 * one retry first (the shared bus does time out now and then); an unconfirmed channel is quarantined at once.
 *
 * Only a device that ANSWERED through the channel ends the quarantine and clears the failure count. A select that
 * merely succeeded does not: a socket that holds the bus does so intermittently (board log 2026-11-13: channel 1
 * hung the bus at 5.8 s, 72 s, 133 s and 254 s because every select that happened to work reset the count, so the
 * wait never grew beyond a minute). A close that fails while a channel is selected is counted against that channel.
 *
 * A channel that never carried a node and has hung the bus NRF_TOPOLOGY_CHANNEL_HANG_LIMIT times is not selected
 * again until the next reboot: every further hang risks a bus that stays dead until the mux is power-cycled, and
 * nothing is lost, as no node ever answered there. The quarantine is not stored: a reboot probes every channel once
 * again.
 */
#define NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS 60000U
#define NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS 3600000U
#define NRF_TOPOLOGY_CHANNEL_HANG_LIMIT 3U
/* The value of the "quarantined until" time of a channel that is disabled until reboot. */
#define NRF_TOPOLOGY_CHANNEL_QUARANTINE_FOREVER_MS UINT64_MAX

typedef enum {
    NRF_TOPOLOGY_NODE_UNKNOWN = 0,
    NRF_TOPOLOGY_NODE_ABSENT,
    NRF_TOPOLOGY_NODE_ASSIGNED,
    NRF_TOPOLOGY_NODE_CONFLICT,
    NRF_TOPOLOGY_NODE_PROTOCOL_ERROR,
    NRF_TOPOLOGY_NODE_TRANSPORT_ERROR,
} nrf_topology_node_state_t;

/* Which check declared NRF_TOPOLOGY_NODE_CONFLICT (diagnostics only). */
typedef enum {
    NRF_TOPOLOGY_CONFLICT_NONE = 0,
    NRF_TOPOLOGY_CONFLICT_REMOTE_ON_DIRECT_BUS,   /* 0x32 answered with the mux closed */
    NRF_TOPOLOGY_CONFLICT_DISCOVERY_AND_MAIN,     /* 0x30 and 0x31 both answered */
    NRF_TOPOLOGY_CONFLICT_MAIN_IDENTITY,          /* 0x31 answered but is not the assigned MAIN */
    NRF_TOPOLOGY_CONFLICT_DISCOVERY_NOT_UNASSIGNED, /* 0x30 answered but is not a ready unassigned node */
    NRF_TOPOLOGY_CONFLICT_REMOTE_DISCOVERY_AND_RUNTIME, /* 0x30 and 0x32 both answered on one channel */
    NRF_TOPOLOGY_CONFLICT_REMOTE_IDENTITY,        /* 0x32 answered but role/node_id/channel/ASSIGNED is wrong */
    NRF_TOPOLOGY_CONFLICT_REMOTE_DISCOVERY_NOT_UNASSIGNED, /* channel 0x30 is not a ready unassigned node */
    NRF_TOPOLOGY_CONFLICT_MAIN_LOST_DURING_SCAN,  /* known MAIN stopped answering while a channel was open */
    NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID,  /* two assigned nodes report the same hardware id */
} nrf_topology_conflict_reason_t;

typedef struct {
    nrf_topology_node_state_t state;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t node_id;
    uint8_t mux_channel;
    uint8_t current_address;
    uint8_t protocol_result;
    uint32_t assignment_request_id;
    /* Raw STATUS fields behind the verdict, so a CONFLICT can be explained from a log. */
    uint8_t role;
    uint8_t lifecycle;
    uint8_t flags;
    uint8_t conflict_reason; /* nrf_topology_conflict_reason_t */
} nrf_topology_node_snapshot_t;

typedef struct {
    uint32_t generation;
    uint32_t age_ms;
    bool tca_available;
    bool tca_fault;
    bool bus_busy;
    bool stale;
    /* Bit N set: channel N was not selected in this pass because it is quarantined (diagnostics only). */
    uint8_t quarantined_channels;
    nrf_topology_node_snapshot_t main;
    nrf_topology_node_snapshot_t remotes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_snapshot_t;

/*
 * Sensor refresh, height-reference capture, IMU calibration and sensor scan only talk to REMOTE nodes
 * through the mux; they never touch MAIN. A snapshot is stale for them only when the bus really was
 * busy or the retained picture is otherwise unusable. A snapshot made stale solely because MAIN failed
 * its own status read (main.state == TRANSPORT_ERROR, the REMOTE states retained from the last good
 * pass) keeps them running: before this, one MAIN read failure refused every calibration start and
 * turned every REMOTE card to "--" until the next reconcile. CS / ranging include MAIN as a node and
 * keep checking `stale` directly.
 */
static inline bool nrf_topology_remote_data_stale(const nrf_topology_snapshot_t *topology)
{
    const bool main_only_fault = topology->main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR &&
        topology->tca_available && !topology->tca_fault;
    return topology->bus_busy || (topology->stale && !main_only_fault);
}

/* MAIN need not have been verified for REMOTE work, but a MAIN in any other odd state still blocks it. */
static inline bool nrf_topology_main_state_allows_remote_work(const nrf_topology_snapshot_t *topology)
{
    return topology->main.state == NRF_TOPOLOGY_NODE_ASSIGNED ||
        topology->main.state == NRF_TOPOLOGY_NODE_ABSENT ||
        (topology->stale && topology->main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR &&
         topology->tca_available && !topology->tca_fault);
}

typedef struct {
    uint32_t confirm_timeout_ms;
    uint32_t poll_interval_ms;
    uint32_t request_seed;
} nrf_topology_config_t;

typedef struct {
    void *context;
    bool tca_available;
    bool tca_fault;
    esp_err_t (*mux_close)(void *context);
    esp_err_t (*mux_select)(void *context, uint8_t channel);
    esp_err_t (*read_status)(void *context, uint8_t address, nrf_upstream_status_t *status);
    esp_err_t (*write_assign)(void *context, uint8_t address, const nrf_upstream_assign_t *assign);
    uint64_t (*now_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t delay_ms);
    bool (*abort_requested)(void *context);
} nrf_topology_io_t;

typedef struct {
    nrf_topology_config_t config;
    uint32_t next_request_id;
    uint32_t generation;
    /* Last confirmed identity per channel. Transport faults must not erase it. */
    nrf_topology_node_snapshot_t last_confirmed_remotes[NRF_TOPOLOGY_REMOTE_COUNT];
    uint64_t remote_confirmed_at_ms[NRF_TOPOLOGY_REMOTE_COUNT];
    bool remote_confirmed_valid[NRF_TOPOLOGY_REMOTE_COUNT];
    /* Channel quarantine: consecutive select failures and the time the channel may be selected again. */
    uint8_t channel_select_failures[NRF_TOPOLOGY_REMOTE_COUNT];
    uint64_t channel_quarantined_until_ms[NRF_TOPOLOGY_REMOTE_COUNT];
    /* The channel carried a confirmed node when it was quarantined: it is reported as an error, not as empty. */
    bool channel_quarantined_with_node[NRF_TOPOLOGY_REMOTE_COUNT];
    /* A device has answered through this channel since boot (never cleared, unlike the retained identity). */
    bool channel_had_device[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_controller_t;

/* Milliseconds until the channel may be selected again; 0 when it is not quarantined. */
uint64_t nrf_topology_channel_quarantine_remaining_ms(
    const nrf_topology_controller_t *controller,
    uint8_t channel,
    uint64_t now_ms);

esp_err_t nrf_topology_controller_init(
    nrf_topology_controller_t *controller,
    const nrf_topology_config_t *config);

esp_err_t nrf_topology_reconcile(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot);
