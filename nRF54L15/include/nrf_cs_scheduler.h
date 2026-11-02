#ifndef NRF_CS_SCHEDULER_H
#define NRF_CS_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_CS_MAX_NODES 5U
#define NRF_CS_MAX_PAIRS 10U
#define NRF_CS_MAX_EXECUTIONS (NRF_CS_MAX_NODES * (NRF_CS_MAX_NODES - 1U))
#define NRF_CS_CONFIDENCE_MAX 1000U

/*
 * MAX-206: a pair collects up to this many accepted samples in one cycle
 * (policy.samples_per_pair per execution; the 2-node mode runs two
 * executions per pair). Distance is the median of the samples that survive
 * a MAD outlier test, not the single first success.
 */
#define NRF_CS_MAX_SAMPLES_PER_PAIR 24U
#define NRF_CS_MAX_SAMPLES_PER_EXECUTION 12U   /* = NRF_CS_CYCLE_MAX_ATTEMPTS: samples + retries of one execution */

/*
 * The two directions of a pair measure the same distance, so their medians must agree. The bench scatter of one
 * sample is 0.16..0.18 m (2026-11-02, nodes 10..20 cm apart); the median of 9 samples then scatters 0.055 m and the
 * difference of two such medians 0.08 m, so 300 mm is about four sigma. Two pairs of the 15:56 run had a total
 * spread (which includes the disagreement) of only 67 and 9 mm, so their directions agreed within that. A pair whose directions differ by more than this is
 * not published as a distance (NRF_CS_PAIR_DIRECTIONS_DISAGREE), because averaging two contradicting values
 * produces a plausible-looking number that is wrong.
 */
#define NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM 300

/* Samples within this band of the median are never rejected, so a very
 * tight cluster (MAD ~ 0) does not throw away millimetre-level jitter. */
#define NRF_CS_MIN_OUTLIER_BAND_MM 100

/*
 * The band is 3 sigma, so the smallest spread a set of samples may claim is a third of it. Without this floor a set in
 * which more than half of the samples are equal (the library gives equal IFFT values, 8490 twice in a row on
 * 2026-11-02) has a MAD of 0 and reports spread 0 even when a third of its samples were rejected as outliers: the
 * 0-4 pair of the 19:55 run had spread_mm=0 with 4 rejected samples (CS-QUALITY-02).
 */
#define NRF_CS_MIN_SPREAD_MM (NRF_CS_MIN_OUTLIER_BAND_MM / 3)

/*
 * Fewest accepted samples one direction needs to count (CS-QUALITY-03). Three is the smallest set whose median can drop
 * one outlier, the size the owner accepted before the move to nine; with one or two samples the "median" is a single
 * value or a mean and there is nothing to reject. The 1-4 pair of the 19:55 run was published as valid from 4 samples
 * of 6 attempts. A policy that asks for fewer samples per pair than this needs only what it asks for.
 */
#define NRF_CS_MIN_SAMPLES_PER_DIRECTION 3U

/*
 * Platform-neutral Channel Sounding cycle core. Bluetooth/NCS adapters feed
 * provider outcomes into this scheduler; the scheduler never owns a link.
 */
typedef uint8_t nrf_cs_node_id_t;

typedef enum {
    NRF_CS_OK = 0,
    NRF_CS_STEP_READY = 1,
    NRF_CS_CYCLE_COMPLETE = 2,
    NRF_CS_ERR_ARGUMENT = -1,
    NRF_CS_ERR_BUSY = -2,
    NRF_CS_ERR_STALE = -3,
    NRF_CS_ERR_NOT_FOUND = -4,
} nrf_cs_result_t;

typedef enum {
    NRF_CS_PAIR_UNKNOWN = 0,
    NRF_CS_PAIR_PENDING,
    NRF_CS_PAIR_VALID,
    NRF_CS_PAIR_MISSING,
    NRF_CS_PAIR_RETRY_EXHAUSTED,
    NRF_CS_PAIR_CABLE_EXCEEDED,
    NRF_CS_PAIR_DIRECTIONS_DISAGREE,
} nrf_cs_pair_state_t;

typedef enum {
    NRF_CS_FAILURE_NONE = 0,
    NRF_CS_FAILURE_TIMEOUT,
    NRF_CS_FAILURE_TRANSPORT,
    NRF_CS_FAILURE_OUTLIER,
    NRF_CS_FAILURE_DISCONNECTED,
} nrf_cs_failure_reason_t;

typedef enum {
    NRF_CS_OUTCOME_SUCCESS = 0,
    NRF_CS_OUTCOME_TIMEOUT,
    NRF_CS_OUTCOME_TRANSPORT_ERROR,
    NRF_CS_OUTCOME_OUTLIER,
} nrf_cs_outcome_t;

typedef enum {
    NRF_CS_QUALITY_NONE = 0,
    NRF_CS_QUALITY_HAD_RETRY = 1U << 0,
    NRF_CS_QUALITY_LOW_CONFIDENCE = 1U << 1,
    NRF_CS_QUALITY_OUTLIER = 1U << 2,
    NRF_CS_QUALITY_TIMEOUT = 1U << 3,
    NRF_CS_QUALITY_TRANSPORT_ERROR = 1U << 4,
    NRF_CS_QUALITY_PARTIAL = 1U << 5,
    NRF_CS_QUALITY_MANUAL = 1U << 6,
} nrf_cs_scheduler_quality_t;

typedef struct {
    uint32_t pair_timeout_ms;
    /* Failed attempts allowed per execution before it gives up. */
    uint8_t max_attempts;
    uint16_t min_confidence;
    /* MAX-206: accepted samples to collect per execution, 1..
     * NRF_CS_MAX_SAMPLES_PER_EXECUTION; 0 is treated as 1. */
    uint8_t samples_per_pair;
    /* Run both directed executions for every pair, in canonical pair order. */
    bool full_rotation;
} nrf_cs_policy_t;

typedef struct {
    nrf_cs_outcome_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
} nrf_cs_measurement_t;

typedef struct {
    nrf_cs_node_id_t node_a;
    nrf_cs_node_id_t node_b;
    nrf_cs_pair_state_t state;
    nrf_cs_failure_reason_t last_failure;
    int32_t distance_mm;
    uint16_t confidence;
    uint16_t provider_quality_flags;
    uint16_t scheduler_quality_flags;
    uint16_t attempts;
    uint8_t valid_samples;
    /* MAX-206: robust spread (1.4826 x MAD) of the accepted samples, in mm;
     * 0 with a single sample. */
    int32_t spread_mm;
    /* Accepted samples discarded as outliers from the median. */
    uint8_t rejected_samples;
    /* Full-rotation result metadata. */
    bool single_direction;
    int32_t direction_disagreement_mm;
    /* Original radio value retained when an operator replaces the distance. */
    int32_t automatic_distance_mm;
} nrf_cs_pair_result_t;

/* A directed execution token for one undirected matrix cell. */
typedef struct {
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t attempt_id;
    uint8_t pair_index;
    uint8_t attempt_number;
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
    uint32_t timeout_ms;
} nrf_cs_step_t;

typedef struct {
    uint8_t pair_index;
    nrf_cs_node_id_t initiator;
    nrf_cs_node_id_t reflector;
    uint8_t attempts;
    uint8_t successes;
    bool terminal;
    bool succeeded;
    nrf_cs_failure_reason_t last_failure;
} nrf_cs_execution_t;

typedef struct {
    nrf_cs_policy_t policy;
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t cycle_membership_generation;
    uint32_t next_attempt_id;
    nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];
    size_t node_count;
    uint8_t disconnected_mask;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    int32_t sample_distance_mm[NRF_CS_MAX_EXECUTIONS][NRF_CS_MAX_SAMPLES_PER_EXECUTION];
    uint16_t sample_confidence[NRF_CS_MAX_EXECUTIONS][NRF_CS_MAX_SAMPLES_PER_EXECUTION];
    size_t pair_count;
    nrf_cs_execution_t executions[NRF_CS_MAX_EXECUTIONS];
    size_t execution_count;
    bool cycle_started;
    bool active;
    size_t active_execution_index;
    nrf_cs_step_t active_step;
} nrf_cs_scheduler_t;

nrf_cs_result_t nrf_cs_scheduler_init(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_policy_t *policy);

nrf_cs_result_t nrf_cs_scheduler_start_cycle(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_node_id_t *node_ids,
    size_t node_count);

nrf_cs_result_t nrf_cs_scheduler_next_step(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_step_t *step_out);

nrf_cs_result_t nrf_cs_scheduler_complete_step(
    nrf_cs_scheduler_t *scheduler,
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement);

nrf_cs_result_t nrf_cs_scheduler_disconnect(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_id);
nrf_cs_result_t nrf_cs_scheduler_abort_cycle(
    nrf_cs_scheduler_t *scheduler,
    nrf_cs_failure_reason_t reason);

bool nrf_cs_scheduler_is_complete(const nrf_cs_scheduler_t *scheduler);
size_t nrf_cs_scheduler_execution_count(const nrf_cs_scheduler_t *scheduler);
size_t nrf_cs_scheduler_executions_done(const nrf_cs_scheduler_t *scheduler);
size_t nrf_cs_scheduler_pair_count(const nrf_cs_scheduler_t *scheduler);
const nrf_cs_pair_result_t *nrf_cs_scheduler_pair_at(
    const nrf_cs_scheduler_t *scheduler,
    size_t pair_index);
const nrf_cs_pair_result_t *nrf_cs_scheduler_matrix_cell(
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b);

#ifdef __cplusplus
}
#endif

#endif
