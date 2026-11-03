#include "ranging_radio_pause_core.h"

#include <string.h>

static uint64_t now_ms(const ranging_radio_core_t *core)
{
    return core->ops.now_ms != NULL ? core->ops.now_ms(core->ops.context) : 0U;
}

void ranging_radio_core_init(ranging_radio_core_t *core, const ranging_radio_ops_t *ops, uint32_t grace_ms, uint32_t retry_ms,
    uint8_t max_start_tries)
{
    if (core == NULL || ops == NULL) return;
    memset(core, 0, sizeof(*core));
    core->ops = *ops;
    core->grace_ms = grace_ms;
    core->retry_ms = retry_ms;
    core->max_start_tries = max_start_tries == 0U ? 1U : max_start_tries;
    core->state = RANGING_RADIO_IDLE;
}

static void announce(ranging_radio_core_t *core)
{
    core->state = RANGING_RADIO_PENDING;
    core->deadline_ms = now_ms(core) + core->grace_ms;
    core->stop_failed = false;
}

void ranging_radio_core_request_off(ranging_radio_core_t *core)
{
    if (core == NULL || core->want_off) return;
    core->want_off = true;
    if (core->state == RANGING_RADIO_IDLE) announce(core);
    /* RESUMING: the start finishes first, step() announces the next stop after it. */
}

void ranging_radio_core_request_on(ranging_radio_core_t *core)
{
    if (core == NULL) return;
    core->want_off = false;
    if (core->state == RANGING_RADIO_PENDING) {
        core->state = RANGING_RADIO_IDLE;           /* never stopped: nothing to give back */
    } else if (core->state == RANGING_RADIO_OFF) {
        core->state = RANGING_RADIO_RESUMING;
        core->start_tries = 0U;
        core->resume_failed = false;
        core->deadline_ms = now_ms(core);
    }
}

static void finish_resume(ranging_radio_core_t *core)
{
    core->state = RANGING_RADIO_IDLE;
    if (core->want_off) announce(core);             /* the next measurement was asked for while the radio came back */
}

void ranging_radio_core_step(ranging_radio_core_t *core)
{
    if (core == NULL) return;
    const uint64_t now = now_ms(core);

    if (core->state == RANGING_RADIO_PENDING && now >= core->deadline_ms) {
        if (!core->want_off) {
            core->state = RANGING_RADIO_IDLE;
        } else if (core->ops.radio_running != NULL && !core->ops.radio_running(core->ops.context)) {
            core->we_stopped = false;                // it was off already: nothing to give back afterwards
            core->state = RANGING_RADIO_OFF;
        } else if (core->ops.radio_stop != NULL && core->ops.radio_stop(core->ops.context) == 0) {
            core->we_stopped = true;
            ++core->stops;
            core->state = RANGING_RADIO_OFF;
        } else {
            core->stop_failed = true;                // still on: the measurement goes ahead rather than waiting for ever
            core->we_stopped = false;
            core->state = RANGING_RADIO_OFF;
        }
    } else if (core->state == RANGING_RADIO_RESUMING && now >= core->deadline_ms) {
        if (!core->we_stopped) {
            finish_resume(core);
        } else if (core->ops.radio_start != NULL && core->ops.radio_start(core->ops.context) == 0) {
            core->we_stopped = false;
            ++core->starts;
            finish_resume(core);
        } else if (++core->start_tries >= core->max_start_tries) {
            core->resume_failed = true;              // the owner restarts the device; the reason stays visible
            core->we_stopped = false;
            finish_resume(core);
        } else {
            core->deadline_ms = now + core->retry_ms;
        }
    }
}

bool ranging_radio_core_ready(const ranging_radio_core_t *core)
{
    return core != NULL && core->state == RANGING_RADIO_OFF;
}

const char *ranging_radio_state_name(ranging_radio_state_t state)
{
    switch (state) {
        case RANGING_RADIO_IDLE: return "idle";
        case RANGING_RADIO_PENDING: return "pending";
        case RANGING_RADIO_OFF: return "off";
        case RANGING_RADIO_RESUMING: return "resuming";
        default: return "?";
    }
}
