#ifndef NRF_CS_TRACE_H
#define NRF_CS_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_distance_method.h"

/*
 * Channel Sounding trace for the UART console.
 *
 * The BLE callbacks and work items only push a small fixed record here (a few stores under a spinlock, no UART and
 * no I2C); the main loop pops the records and prints them. The records do not go through the nRF log ring that the
 * ESP32 reads over I2C, so watching a ranging cycle on the console adds nothing to the bus.
 *
 * What it is for: the ESP32 log only shows how an attempt ended ("timeout in SECURING"); to set the time budget of
 * one pair the time of every stage (connect, security, MTU, discovery, CS configuration, the procedure itself) is
 * needed, and each line carries the milliseconds since the attempt started.
 */

#define NRF_CS_TRACE_CAPACITY 48U
#define NRF_CS_TRACE_LINE_CAPACITY 240U
#define NRF_CS_TRACE_NO_ELAPSED UINT32_MAX

typedef enum {
    NRF_CS_TRACE_EXECUTE = 1,   /* an attempt was accepted: budget and who measures whom */
    NRF_CS_TRACE_EVENT,         /* a runtime event arrived while the attempt was in `phase` */
    NRF_CS_TRACE_TIMEOUT,       /* the attempt budget ran out in `phase` */
    NRF_CS_TRACE_RESULT,        /* the attempt ended: outcome, distance, confidence */
    NRF_CS_TRACE_RF,            /* the radio summary words of the attempt (decoded by the ESP32 log) */
    NRF_CS_TRACE_ESTIMATES,     /* the three distance estimates of the library and the one picked as "best" */
} nrf_cs_trace_kind_t;

/* An estimate the library could not make (not finite, negative): printed as n/a. */
#define NRF_CS_TRACE_UNAVAILABLE NRF_CS_DISTANCE_UNAVAILABLE

/* nrf_cs_trace_entry_t.flags of a RESULT: c and d carry the timing of the attempt (CS-TIME-01). */
#define NRF_CS_TRACE_FLAG_TIMING (1U << 0)              /* c = ms until the outcome was known, d = ms of cleanup */
#define NRF_CS_TRACE_FLAG_OVER_BUDGET (1U << 1)         /* the whole attempt took longer than its budget */
#define NRF_CS_TRACE_FLAG_TIMEOUT_IN_CLEANUP (1U << 2)  /* the budget timer fired during cleanup and was ignored */
/* nrf_cs_trace_entry_t.flags of an ESTIMATES: `e` holds the library's own IFFT (before the gap fill). */
#define NRF_CS_TRACE_FLAG_STOCK_IFFT (1U << 3)

typedef struct {
    uint32_t t_ms;              /* uptime when the record was pushed */
    uint32_t elapsed_ms;        /* since the attempt was accepted, or NRF_CS_TRACE_NO_ELAPSED */
    uint32_t attempt_id;
    const char *label;          /* a string literal: event name or outcome name */
    int32_t a;
    int32_t b;
    int32_t c;                  /* ESTIMATES: rtt, in mm. RESULT with FLAG_TIMING: measured_ms */
    int32_t d;                  /* ESTIMATES: best, in mm. RESULT with FLAG_TIMING: cleanup_ms */
    int32_t e;                  /* ESTIMATES: the library's own IFFT before the gap fill, in mm; NRF_CS_TRACE_UNAVAILABLE = none */
    uint8_t kind;
    uint8_t phase;              /* nrf_cs_pair_phase_t */
    uint8_t flags;              /* RESULT: NRF_CS_TRACE_FLAG_* */
    uint8_t tones_planned;      /* ESTIMATES: channels in the map; 0 = not known, the tone fields are not printed */
    uint8_t tones_usable;       /* ESTIMATES: planned channels with a good tone on both sides */
    uint8_t tones_gap;          /* ESTIMATES: longest run of slots without a usable tone */
    uint8_t tones_lead;         /* ESTIMATES: slots without a usable tone at the start of the array */
    uint8_t tones_tail;         /* ESTIMATES: the same at the end */
    uint16_t peak_floor_x10;    /* ESTIMATES: IFFT peak over the median bin, times 10; 0 = not known (see nrf_cs_ifft_quality.h) */
    const char *why;            /* ESTIMATES: nrf_cs_confidence_reason_name() of the confidence (a string literal); NULL = not known */
} nrf_cs_trace_entry_t;

typedef struct {
    nrf_cs_trace_entry_t entries[NRF_CS_TRACE_CAPACITY];
    uint32_t head;              /* next to write */
    uint32_t tail;              /* next to read */
    uint32_t dropped;           /* pushes that found the ring full, since the last take */
} nrf_cs_trace_t;

void nrf_cs_trace_init(nrf_cs_trace_t *trace);
/* False when the ring is full: the new record is dropped (and counted), the ones already queued keep their order. */
bool nrf_cs_trace_push(nrf_cs_trace_t *trace, const nrf_cs_trace_entry_t *entry);
bool nrf_cs_trace_pop(nrf_cs_trace_t *trace, nrf_cs_trace_entry_t *entry);
/* Records dropped since the previous call. */
uint32_t nrf_cs_trace_take_dropped(nrf_cs_trace_t *trace);

const char *nrf_cs_trace_phase_name(uint8_t phase);
const char *nrf_cs_trace_outcome_name(uint8_t outcome);
/*
 * The reason behind the status of a TRANSPORT_ERROR event, or NULL when it has no name. In SECURING the status is
 * Zephyr's enum bt_security_err (NCS v3.4.0, zephyr/include/zephyr/bluetooth/conn.h): 9 is a pairing that failed for
 * a reason the stack did not name, not a Channel Sounding timeout.
 */
const char *nrf_cs_trace_status_reason(const char *event_label, uint8_t phase, int status);

/* One console line without the newline; the length, or -1 for a bad argument or an unknown kind. */
int nrf_cs_trace_format(const nrf_cs_trace_entry_t *entry, char *line, size_t capacity);
int nrf_cs_trace_format_dropped(uint32_t dropped, char *line, size_t capacity);

#endif
