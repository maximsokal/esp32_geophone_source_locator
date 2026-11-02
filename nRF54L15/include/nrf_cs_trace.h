#ifndef NRF_CS_TRACE_H
#define NRF_CS_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#define NRF_CS_TRACE_LINE_CAPACITY 128U
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
#define NRF_CS_TRACE_UNAVAILABLE INT32_MIN

typedef struct {
    uint32_t t_ms;              /* uptime when the record was pushed */
    uint32_t elapsed_ms;        /* since the attempt was accepted, or NRF_CS_TRACE_NO_ELAPSED */
    uint32_t attempt_id;
    const char *label;          /* a string literal: event name or outcome name */
    int32_t a;
    int32_t b;
    int32_t c;                  /* ESTIMATES: rtt, in mm */
    int32_t d;                  /* ESTIMATES: best, in mm */
    uint8_t kind;
    uint8_t phase;              /* nrf_cs_pair_phase_t */
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

/* One console line without the newline; the length, or -1 for a bad argument or an unknown kind. */
int nrf_cs_trace_format(const nrf_cs_trace_entry_t *entry, char *line, size_t capacity);
int nrf_cs_trace_format_dropped(uint32_t dropped, char *line, size_t capacity);

#endif
