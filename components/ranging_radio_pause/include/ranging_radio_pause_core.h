#ifndef RANGING_RADIO_PAUSE_CORE_H
#define RANGING_RADIO_PAUSE_CORE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Wi-Fi off while the distances are measured (platform neutral).
 *
 * The ESP32 access point is 13 cm from the MAIN nRF and sits on 2.4 GHz channel 1, which covers the first ~20 of the 75 Channel
 * Sounding channels. While the nodes range, the radio is stopped; it is started again by itself when the measurement is over
 * (to the saved phone network, or the setup access point if none is saved: wifi_manager_start() decides).
 *
 *   IDLE      nothing asked; the radio is as it was
 *   PENDING   the measurement page was entered; the radio is still on for `grace_ms` so the screen and the web page can say
 *             that it is about to go off
 *   OFF       the radio is stopped (or was not running); the measurement may start
 *   RESUMING  the measurement is over; the radio is being started again, retried when the start is refused
 *
 * The core never blocks on its own: step() performs the stop and the start, so the caller runs it from a task of its own, never
 * from the web server or the UI.
 */

typedef enum {
    RANGING_RADIO_IDLE = 0,
    RANGING_RADIO_PENDING,
    RANGING_RADIO_OFF,
    RANGING_RADIO_RESUMING,
} ranging_radio_state_t;

typedef struct {
    bool (*radio_running)(void *context);
    int (*radio_stop)(void *context);       /* 0 = stopped */
    int (*radio_start)(void *context);      /* 0 = started */
    uint64_t (*now_ms)(void *context);
    void *context;
} ranging_radio_ops_t;

typedef struct {
    ranging_radio_ops_t ops;
    uint32_t grace_ms;
    uint32_t retry_ms;
    uint8_t max_start_tries;
    ranging_radio_state_t state;
    bool want_off;
    bool we_stopped;        /* this module stopped the radio, so it owes a start */
    bool stop_failed;       /* the last stop was refused: the radio stayed on and the measurement goes ahead */
    bool resume_failed;     /* the start was refused max_start_tries times in a row */
    uint64_t deadline_ms;
    uint8_t start_tries;
    uint32_t stops;
    uint32_t starts;
} ranging_radio_core_t;

void ranging_radio_core_init(ranging_radio_core_t *core, const ranging_radio_ops_t *ops, uint32_t grace_ms, uint32_t retry_ms,
    uint8_t max_start_tries);

/* The measurement page was entered / left. Idempotent. */
void ranging_radio_core_request_off(ranging_radio_core_t *core);
void ranging_radio_core_request_on(ranging_radio_core_t *core);

/* Performs what is due (the stop after the grace time, the start). */
void ranging_radio_core_step(ranging_radio_core_t *core);

/* True when the measurement may start: the radio is off (or the stop was refused: the measurement is not held back for ever). */
bool ranging_radio_core_ready(const ranging_radio_core_t *core);

const char *ranging_radio_state_name(ranging_radio_state_t state);

#endif
