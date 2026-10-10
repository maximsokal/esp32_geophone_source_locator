#ifndef NRF_STAGE_SIGNAL_H
#define NRF_STAGE_SIGNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_STAGE_SIGNAL_QUEUE_CAPACITY 4U

typedef enum {
    NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE = 0,
    NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE,
    NRF_STAGE_SIGNAL_EVENT_SENSORS_READY,
    NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED,
    NRF_STAGE_SIGNAL_EVENT_COUNT,
} nrf_stage_signal_event_t;

typedef enum {
    NRF_STAGE_SIGNAL_OK = 0,
    NRF_STAGE_SIGNAL_ERR_ARGUMENT = -1,
    NRF_STAGE_SIGNAL_ERR_QUEUE_FULL = -2,
    NRF_STAGE_SIGNAL_ERR_NO_EVENT = -3,
} nrf_stage_signal_result_t;

typedef struct {
    bool overlay_active;
    bool green;
    bool yellow;
    bool red;
} nrf_stage_signal_output_t;

typedef struct {
    nrf_stage_signal_event_t pending_events[NRF_STAGE_SIGNAL_QUEUE_CAPACITY];
    uint8_t queue_head;
    uint8_t queue_count;
    bool event_active;
    nrf_stage_signal_event_t active_event;
    uint8_t flash_count;
    uint8_t flash_index;
    uint8_t phase;
    uint32_t phase_remaining_ms;
    uint32_t event_last_ms;
    bool started_event_pending;
    nrf_stage_signal_event_t started_event;
    bool suppressed;
    bool last_update_suppressed;
    bool has_transactions;
    uint32_t last_transaction_count;
    uint32_t last_transaction_change_ms;
    uint32_t loss_timeout_ms;
    bool link_lost;
    uint32_t lost_since_ms;
    uint32_t lost_phase_ms;
    uint32_t lost_phase_cursor_ms;
    bool has_lost_phase_cursor;
    uint32_t dropped_total;
} nrf_stage_signal_t;

nrf_stage_signal_result_t nrf_stage_signal_init(
    nrf_stage_signal_t *state,
    uint32_t loss_timeout_ms);
nrf_stage_signal_result_t nrf_stage_signal_post_event(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t event,
    uint32_t now_ms);
nrf_stage_signal_result_t nrf_stage_signal_note_transactions(
    nrf_stage_signal_t *state,
    uint32_t transaction_count,
    uint32_t now_ms);
nrf_stage_signal_result_t nrf_stage_signal_set_suppressed(
    nrf_stage_signal_t *state,
    bool suppressed);
nrf_stage_signal_result_t nrf_stage_signal_update(
    nrf_stage_signal_t *state,
    uint32_t now_ms,
    nrf_stage_signal_output_t *out);
nrf_stage_signal_result_t nrf_stage_signal_started_event(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t *event);
bool nrf_stage_signal_link_lost(const nrf_stage_signal_t *state);
uint32_t nrf_stage_signal_dropped_total(const nrf_stage_signal_t *state);
const char *nrf_stage_signal_event_name(nrf_stage_signal_event_t event);

#ifdef __cplusplus
}
#endif

#endif
