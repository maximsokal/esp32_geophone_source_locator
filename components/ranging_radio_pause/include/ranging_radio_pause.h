#ifndef RANGING_RADIO_PAUSE_H
#define RANGING_RADIO_PAUSE_H

#include <stdbool.h>

#include "esp_err.h"
#include "ranging_radio_pause_core.h"

/*
 * Wi-Fi off while the distances are measured, back on by itself afterwards (see ranging_radio_pause_core.h). The requests only
 * record what is wanted; a task of its own stops and starts the radio, so they are safe to call from the workflow poll, the web
 * server and the UI. Before ranging_radio_pause_init() (host tests, a build without it) nothing is ever held back.
 */

esp_err_t ranging_radio_pause_init(void);

/* The measurement page was entered: announce, then switch the radio off. Idempotent. */
void ranging_radio_pause_request_off(void);
/* The measurement is over, or the page was left: give the radio back. Idempotent. */
void ranging_radio_pause_request_on(void);

/* True when the measurement may start. Always true when the module is not running. */
bool ranging_radio_pause_ready(void);
ranging_radio_state_t ranging_radio_pause_state(void);
/* The last stop was refused (the radio stayed on) / the start was refused every time. */
bool ranging_radio_pause_stop_failed(void);
bool ranging_radio_pause_resume_failed(void);

#endif
