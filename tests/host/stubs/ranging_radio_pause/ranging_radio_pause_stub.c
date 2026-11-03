#include "ranging_radio_pause.h"

bool g_radio_pause_ready = true;
unsigned g_radio_pause_off_requests;
unsigned g_radio_pause_on_requests;
bool g_radio_pause_wants_off;
ranging_radio_state_t g_radio_pause_state = RANGING_RADIO_IDLE;
bool g_radio_pause_stop_failed;
bool g_radio_pause_resume_failed;

void ranging_radio_pause_request_off(void)
{
    ++g_radio_pause_off_requests;
    g_radio_pause_wants_off = true;
}

void ranging_radio_pause_request_on(void)
{
    ++g_radio_pause_on_requests;
    g_radio_pause_wants_off = false;
}

bool ranging_radio_pause_ready(void)
{
    return g_radio_pause_ready;
}

ranging_radio_state_t ranging_radio_pause_state(void)
{
    return g_radio_pause_state;
}

bool ranging_radio_pause_stop_failed(void)
{
    return g_radio_pause_stop_failed;
}

bool ranging_radio_pause_resume_failed(void)
{
    return g_radio_pause_resume_failed;
}
