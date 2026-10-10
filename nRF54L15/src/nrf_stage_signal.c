#include "nrf_stage_signal.h"

#define NRF_STAGE_SIGNAL_LEADING_MS UINT32_C(300)
#define NRF_STAGE_SIGNAL_FLASH_ON_MS UINT32_C(150)
#define NRF_STAGE_SIGNAL_FLASH_OFF_MS UINT32_C(150)
#define NRF_STAGE_SIGNAL_TRAILING_MS UINT32_C(300)
#define NRF_STAGE_SIGNAL_LOST_PERIOD_MS UINT32_C(2000)
#define NRF_STAGE_SIGNAL_LOST_ON_MS UINT32_C(150)

enum {
    NRF_STAGE_SIGNAL_PHASE_LEADING_OFF = 0,
    NRF_STAGE_SIGNAL_PHASE_FLASH_ON,
    NRF_STAGE_SIGNAL_PHASE_FLASH_OFF,
    NRF_STAGE_SIGNAL_PHASE_TRAILING_OFF,
};

static uint8_t nrf_stage_signal_flash_count(nrf_stage_signal_event_t event)
{
    switch (event) {
    case NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE:
        return 1U;
    case NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE:
        return 2U;
    case NRF_STAGE_SIGNAL_EVENT_SENSORS_READY:
    case NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED:
        return 3U;
    default:
        return 0U;
    }
}

static bool nrf_stage_signal_valid_event(nrf_stage_signal_event_t event)
{
    int32_t value = (int32_t)event;
    return value >= 0 && value < (int32_t)NRF_STAGE_SIGNAL_EVENT_COUNT;
}

static void nrf_stage_signal_increment_dropped(nrf_stage_signal_t *state)
{
    if (state->dropped_total < UINT32_MAX) {
        ++state->dropped_total;
    }
}

static void nrf_stage_signal_start_event(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t event,
    uint32_t now_ms)
{
    state->event_active = true;
    state->active_event = event;
    state->flash_count = nrf_stage_signal_flash_count(event);
    state->flash_index = 0U;
    state->phase = NRF_STAGE_SIGNAL_PHASE_LEADING_OFF;
    state->phase_remaining_ms = NRF_STAGE_SIGNAL_LEADING_MS;
    state->event_last_ms = now_ms;
    state->started_event = event;
    state->started_event_pending = true;
}

static void nrf_stage_signal_start_next_event(
    nrf_stage_signal_t *state,
    uint32_t now_ms)
{
    nrf_stage_signal_event_t event = state->pending_events[state->queue_head];

    state->queue_head = (uint8_t)(
        (state->queue_head + 1U) % NRF_STAGE_SIGNAL_QUEUE_CAPACITY);
    --state->queue_count;
    nrf_stage_signal_start_event(state, event, now_ms);
}

static void nrf_stage_signal_advance_event(
    nrf_stage_signal_t *state,
    uint32_t elapsed_ms,
    uint32_t now_ms)
{
    while (state->event_active && elapsed_ms >= state->phase_remaining_ms) {
        elapsed_ms -= state->phase_remaining_ms;
        switch (state->phase) {
        case NRF_STAGE_SIGNAL_PHASE_LEADING_OFF:
            state->phase = NRF_STAGE_SIGNAL_PHASE_FLASH_ON;
            state->phase_remaining_ms = NRF_STAGE_SIGNAL_FLASH_ON_MS;
            break;
        case NRF_STAGE_SIGNAL_PHASE_FLASH_ON:
            state->phase = NRF_STAGE_SIGNAL_PHASE_FLASH_OFF;
            state->phase_remaining_ms = NRF_STAGE_SIGNAL_FLASH_OFF_MS;
            break;
        case NRF_STAGE_SIGNAL_PHASE_FLASH_OFF:
            ++state->flash_index;
            if (state->flash_index < state->flash_count) {
                state->phase = NRF_STAGE_SIGNAL_PHASE_FLASH_ON;
                state->phase_remaining_ms = NRF_STAGE_SIGNAL_FLASH_ON_MS;
            } else {
                state->phase = NRF_STAGE_SIGNAL_PHASE_TRAILING_OFF;
                state->phase_remaining_ms = NRF_STAGE_SIGNAL_TRAILING_MS;
            }
            break;
        case NRF_STAGE_SIGNAL_PHASE_TRAILING_OFF:
            state->event_active = false;
            state->phase_remaining_ms = 0U;
            if (state->queue_count != 0U) {
                nrf_stage_signal_start_next_event(state, now_ms - elapsed_ms);
            }
            break;
        default:
            state->event_active = false;
            state->phase_remaining_ms = 0U;
            break;
        }
    }

    if (state->event_active) {
        state->phase_remaining_ms -= elapsed_ms;
        state->event_last_ms = now_ms;
    }
}

static void nrf_stage_signal_add_lost_elapsed(
    nrf_stage_signal_t *state,
    uint32_t elapsed_ms)
{
    uint32_t remainder = elapsed_ms % NRF_STAGE_SIGNAL_LOST_PERIOD_MS;
    state->lost_phase_ms =
        (state->lost_phase_ms + remainder) % NRF_STAGE_SIGNAL_LOST_PERIOD_MS;
}

static void nrf_stage_signal_refresh_link_lost(
    nrf_stage_signal_t *state,
    uint32_t now_ms,
    bool clock_paused)
{
    if (!state->has_transactions) {
        return;
    }

    if (!state->link_lost &&
        (uint32_t)(now_ms - state->last_transaction_change_ms) >=
            state->loss_timeout_ms) {
        state->link_lost = true;
        state->lost_since_ms =
            state->last_transaction_change_ms + state->loss_timeout_ms;
        state->lost_phase_ms = 0U;
        state->lost_phase_cursor_ms = state->lost_since_ms;
        state->has_lost_phase_cursor = true;
    }

    if (!state->link_lost) {
        return;
    }

    if (!state->has_lost_phase_cursor) {
        state->lost_phase_cursor_ms = now_ms;
        state->has_lost_phase_cursor = true;
        return;
    }

    if (!clock_paused) {
        nrf_stage_signal_add_lost_elapsed(
            state, (uint32_t)(now_ms - state->lost_phase_cursor_ms));
    }
    state->lost_phase_cursor_ms = now_ms;
}

nrf_stage_signal_result_t nrf_stage_signal_init(
    nrf_stage_signal_t *state,
    uint32_t loss_timeout_ms)
{
    if (state == NULL || loss_timeout_ms == 0U) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }

    *state = (nrf_stage_signal_t){0};
    state->loss_timeout_ms = loss_timeout_ms;
    return NRF_STAGE_SIGNAL_OK;
}

nrf_stage_signal_result_t nrf_stage_signal_post_event(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t event,
    uint32_t now_ms)
{
    uint8_t tail;

    if (state == NULL || !nrf_stage_signal_valid_event(event)) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }

    if (!state->event_active && !state->suppressed && state->queue_count == 0U) {
        nrf_stage_signal_start_event(state, event, now_ms);
        return NRF_STAGE_SIGNAL_OK;
    }

    if (state->queue_count >= NRF_STAGE_SIGNAL_QUEUE_CAPACITY) {
        nrf_stage_signal_increment_dropped(state);
        return NRF_STAGE_SIGNAL_ERR_QUEUE_FULL;
    }

    tail = (uint8_t)(
        (state->queue_head + state->queue_count) % NRF_STAGE_SIGNAL_QUEUE_CAPACITY);
    state->pending_events[tail] = event;
    ++state->queue_count;
    return NRF_STAGE_SIGNAL_OK;
}

nrf_stage_signal_result_t nrf_stage_signal_note_transactions(
    nrf_stage_signal_t *state,
    uint32_t transaction_count,
    uint32_t now_ms)
{
    if (state == NULL) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }

    if (!state->has_transactions) {
        if (transaction_count == 0U) {
            return NRF_STAGE_SIGNAL_OK;
        }

        state->has_transactions = true;
        state->last_transaction_count = transaction_count;
        state->last_transaction_change_ms = now_ms;
        state->link_lost = false;
        state->lost_phase_ms = 0U;
        state->has_lost_phase_cursor = false;
        return nrf_stage_signal_post_event(
            state, NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE, now_ms);
    }

    if (transaction_count != state->last_transaction_count) {
        state->last_transaction_count = transaction_count;
        state->last_transaction_change_ms = now_ms;
        state->link_lost = false;
        state->lost_phase_ms = 0U;
        state->has_lost_phase_cursor = false;
        return NRF_STAGE_SIGNAL_OK;
    }

    nrf_stage_signal_refresh_link_lost(
        state,
        now_ms,
        state->suppressed || state->last_update_suppressed ||
            state->event_active);
    return NRF_STAGE_SIGNAL_OK;
}

nrf_stage_signal_result_t nrf_stage_signal_set_suppressed(
    nrf_stage_signal_t *state,
    bool suppressed)
{
    if (state == NULL) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }

    state->suppressed = suppressed;
    return NRF_STAGE_SIGNAL_OK;
}

nrf_stage_signal_result_t nrf_stage_signal_update(
    nrf_stage_signal_t *state,
    uint32_t now_ms,
    nrf_stage_signal_output_t *out)
{
    bool sequencer_paused;
    bool lost_flash_paused;

    if (state == NULL || out == NULL) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }

    sequencer_paused = state->suppressed || state->last_update_suppressed;
    lost_flash_paused = sequencer_paused || state->event_active;
    nrf_stage_signal_refresh_link_lost(state, now_ms, lost_flash_paused);

    if (state->event_active) {
        uint32_t elapsed_ms = (uint32_t)(now_ms - state->event_last_ms);
        if (sequencer_paused) {
            state->event_last_ms = now_ms;
        } else {
            nrf_stage_signal_advance_event(state, elapsed_ms, now_ms);
        }
    } else if (!state->suppressed && state->queue_count != 0U) {
        nrf_stage_signal_start_next_event(state, now_ms);
    }

    out->overlay_active = false;
    out->green = false;
    out->yellow = false;
    out->red = false;

    if (!state->suppressed) {
        if (state->event_active) {
            out->overlay_active = true;
            if (state->phase == NRF_STAGE_SIGNAL_PHASE_FLASH_ON) {
                if (state->active_event == NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED) {
                    out->red = true;
                } else {
                    out->green = true;
                }
            }
        } else if (state->link_lost &&
                   state->lost_phase_ms < NRF_STAGE_SIGNAL_LOST_ON_MS) {
            out->overlay_active = true;
            out->yellow = true;
        }
    }

    state->last_update_suppressed = state->suppressed;
    return NRF_STAGE_SIGNAL_OK;
}

nrf_stage_signal_result_t nrf_stage_signal_started_event(
    nrf_stage_signal_t *state,
    nrf_stage_signal_event_t *event)
{
    if (state == NULL || event == NULL) {
        return NRF_STAGE_SIGNAL_ERR_ARGUMENT;
    }
    if (!state->started_event_pending) {
        return NRF_STAGE_SIGNAL_ERR_NO_EVENT;
    }

    *event = state->started_event;
    state->started_event_pending = false;
    return NRF_STAGE_SIGNAL_OK;
}

bool nrf_stage_signal_link_lost(const nrf_stage_signal_t *state)
{
    return state != NULL && state->link_lost;
}

uint32_t nrf_stage_signal_dropped_total(const nrf_stage_signal_t *state)
{
    return state != NULL ? state->dropped_total : 0U;
}

const char *nrf_stage_signal_event_name(nrf_stage_signal_event_t event)
{
    switch (event) {
    case NRF_STAGE_SIGNAL_EVENT_I2C_ALIVE:
        return "I2C_ALIVE";
    case NRF_STAGE_SIGNAL_EVENT_ROLE_REMOTE:
        return "ROLE_REMOTE";
    case NRF_STAGE_SIGNAL_EVENT_SENSORS_READY:
        return "SENSORS_READY";
    case NRF_STAGE_SIGNAL_EVENT_SENSORS_FAILED:
        return "SENSORS_FAILED";
    default:
        return "UNKNOWN";
    }
}
