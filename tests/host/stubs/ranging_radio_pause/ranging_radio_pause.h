#ifndef RANGING_RADIO_PAUSE_H
#define RANGING_RADIO_PAUSE_H

#include <stdbool.h>

#include "ranging_radio_pause_core.h"

/* Host stand-in for the Wi-Fi pause of the distance measurement: records the requests, and the test says when the radio is "off". */
void ranging_radio_pause_request_off(void);
void ranging_radio_pause_request_on(void);
bool ranging_radio_pause_ready(void);
ranging_radio_state_t ranging_radio_pause_state(void);
bool ranging_radio_pause_stop_failed(void);
bool ranging_radio_pause_resume_failed(void);

extern bool g_radio_pause_ready;            /* default true: the radio is off at once, as without the module */
extern unsigned g_radio_pause_off_requests;
extern unsigned g_radio_pause_on_requests;
extern bool g_radio_pause_wants_off;        /* what was asked last */
extern ranging_radio_state_t g_radio_pause_state;
extern bool g_radio_pause_stop_failed;
extern bool g_radio_pause_resume_failed;

#endif
