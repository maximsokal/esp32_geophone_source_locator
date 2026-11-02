#ifndef NRF_CS_CYCLE_PROTOCOL_H
#define NRF_CS_CYCLE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Autonomous Channel Sounding cycle: the ESP32 hands every node the plan, starts
 * the first initiator once, and then keeps the shared I2C bus silent while the
 * nodes pass the turn to each other over Bluetooth. MAIN reports the outcome.
 *
 * Every frame ends with the same CRC-16/CCITT-FALSE as the other nRF frames. Frames
 * the ESP32 writes stay at 30 bytes or less: the ESP-IDF master moves longer writes in
 * FIFO segments and the 32 byte CS_EXECUTE was truncated on the bench
 * (2026-10-27, nRF read 18-19 of 32 bytes).
 */
#define NRF_CS_CYCLE_MESSAGE_PLAN 0x0BU
#define NRF_CS_CYCLE_MESSAGE_PLAN_STEPS 0x0CU
#define NRF_CS_CYCLE_MESSAGE_GO 0x0DU
#define NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST 0x0EU
#define NRF_CS_CYCLE_MESSAGE_STATUS 0x85U
#define NRF_CS_CYCLE_MESSAGE_RESULT 0x86U

#define NRF_CS_CYCLE_MAX_STEPS NRF_CS_MAX_EXECUTIONS
#define NRF_CS_CYCLE_STEPS_PER_CHUNK 6U
#define NRF_CS_CYCLE_RESULTS_PER_PAGE 8U
/* Most attempts one execution can use: samples_per_pair successes plus max_attempts - 1 failures. */
#define NRF_CS_CYCLE_MAX_ATTEMPTS 12U
#define NRF_CS_CYCLE_MAX_RECORDS (NRF_CS_CYCLE_MAX_STEPS * NRF_CS_CYCLE_MAX_ATTEMPTS)
/* RESULT_REQUEST page that tells the node the ESP32 has everything and it may serve STATUS again. */
#define NRF_CS_CYCLE_RESULT_RELEASE_PAGE 0xFFU

#define NRF_CS_CYCLE_PLAN_LENGTH 28U
#define NRF_CS_CYCLE_PLAN_STEPS_LENGTH 30U
#define NRF_CS_CYCLE_GO_LENGTH 20U
#define NRF_CS_CYCLE_RESULT_REQUEST_LENGTH 18U
/* The status reply is one 28 byte window, like the legacy STATUS. */
#define NRF_CS_CYCLE_STATUS_LENGTH 28U
#define NRF_CS_CYCLE_RESULT_RECORD_LENGTH 12U
#define NRF_CS_CYCLE_RESULT_LENGTH \
    (20U + NRF_CS_CYCLE_RESULTS_PER_PAGE * NRF_CS_CYCLE_RESULT_RECORD_LENGTH)

typedef enum {
    NRF_CS_CYCLE_STATE_IDLE = 0,
    NRF_CS_CYCLE_STATE_RUNNING = 1,
    NRF_CS_CYCLE_STATE_COMPLETE = 2,
    NRF_CS_CYCLE_STATE_FAILED = 3,
} nrf_cs_cycle_state_t;

typedef enum {
    NRF_CS_CYCLE_FAILURE_NONE = 0,
    NRF_CS_CYCLE_FAILURE_TIMEOUT = 1,
    NRF_CS_CYCLE_FAILURE_BLE = 2,
    NRF_CS_CYCLE_FAILURE_PLAN = 3,
    NRF_CS_CYCLE_FAILURE_ABORTED = 4,
} nrf_cs_cycle_failure_t;

/* Policy and sizes; the directed steps follow in PLAN_STEPS chunks. */
typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint16_t membership_generation;
    uint16_t pair_timeout_ms;
    uint8_t max_attempts;
    uint8_t samples_per_pair;
    uint16_t min_confidence;
    bool full_rotation;
    uint8_t step_count;
    uint8_t node_count;
    uint8_t self_node;
} nrf_cs_cycle_plan_t;

typedef struct {
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
} nrf_cs_cycle_step_t;

typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint8_t first_step;
    uint8_t step_count;
    nrf_cs_cycle_step_t steps[NRF_CS_CYCLE_STEPS_PER_CHUNK];
} nrf_cs_cycle_plan_steps_t;

typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint32_t total_timeout_ms;
} nrf_cs_cycle_go_t;

typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint8_t state;
    uint8_t failure;
    uint8_t pairs_done;
    uint8_t pairs_total;
    uint32_t elapsed_ms;
} nrf_cs_cycle_status_t;

typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint8_t page;
} nrf_cs_cycle_result_request_t;

/*
 * One attempt a node made as initiator. The ESP32 replays these through the normal
 * scheduler (execution order = plan order), so retries, sample medians and pair
 * results come out exactly as if it had driven every step itself.
 */
typedef struct {
    uint8_t execution;
    uint8_t attempt;
    uint8_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
} nrf_cs_cycle_record_t;

typedef struct {
    uint32_t request_id;
    uint32_t cycle_id;
    uint8_t page;
    uint8_t page_count;
    uint8_t record_total;
    uint8_t record_count;
    nrf_cs_cycle_record_t records[NRF_CS_CYCLE_RESULTS_PER_PAGE];
} nrf_cs_cycle_result_t;

/* All functions return 0 on success and a negative value for an invalid or corrupt frame. */
int nrf_cs_cycle_encode_plan(const nrf_cs_cycle_plan_t *plan, uint8_t frame[NRF_CS_CYCLE_PLAN_LENGTH]);
int nrf_cs_cycle_decode_plan(const uint8_t *frame, size_t length, nrf_cs_cycle_plan_t *plan);
int nrf_cs_cycle_encode_plan_steps(const nrf_cs_cycle_plan_steps_t *chunk, uint8_t frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH]);
int nrf_cs_cycle_decode_plan_steps(const uint8_t *frame, size_t length, nrf_cs_cycle_plan_steps_t *chunk);
int nrf_cs_cycle_encode_go(const nrf_cs_cycle_go_t *go, uint8_t frame[NRF_CS_CYCLE_GO_LENGTH]);
int nrf_cs_cycle_decode_go(const uint8_t *frame, size_t length, nrf_cs_cycle_go_t *go);
int nrf_cs_cycle_encode_result_request(const nrf_cs_cycle_result_request_t *request, uint8_t frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH]);
int nrf_cs_cycle_decode_result_request(const uint8_t *frame, size_t length, nrf_cs_cycle_result_request_t *request);
int nrf_cs_cycle_encode_status(const nrf_cs_cycle_status_t *status, uint8_t frame[NRF_CS_CYCLE_STATUS_LENGTH]);
int nrf_cs_cycle_decode_status(const uint8_t *frame, size_t length, nrf_cs_cycle_status_t *status);
int nrf_cs_cycle_encode_result(const nrf_cs_cycle_result_t *result, uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH]);
int nrf_cs_cycle_decode_result(const uint8_t *frame, size_t length, nrf_cs_cycle_result_t *result);

/* Number of PLAN_STEPS chunks and result pages for the given counts. */
size_t nrf_cs_cycle_plan_chunk_count(size_t step_count);
size_t nrf_cs_cycle_result_page_count(size_t pair_count);

void nrf_cs_cycle_record_from_measurement(
    uint8_t execution, uint8_t attempt, const nrf_cs_measurement_t *measurement, nrf_cs_cycle_record_t *record);
void nrf_cs_cycle_record_to_measurement(const nrf_cs_cycle_record_t *record, nrf_cs_measurement_t *measurement);

/*
 * One log line (no newline) for an attempt record: "exec=3 attempt=2 outcome=SUCCESS dist_mm=6194 conf=755 method=ifft
 * tone_ok=1 pbr=1 rtt=1". The method is the library estimate the distance was taken from (CS-ESTIMATOR-01); it is "none"
 * for an attempt without a distance. Returns the length like snprintf, or -1 for a bad argument.
 */
int nrf_cs_cycle_record_describe(const nrf_cs_cycle_record_t *record, char *line, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
