/*
 * Replays the attempts of a real ranging cycle through the production scheduler with the production policy.
 *
 * Input: the CSV of the bench cycle of 2026-11-03 (tests/host/fixtures/bench_2026_11_03/ranging_samples.csv, the RANGING_SAMPLE lines
 * of the ESP32 log): one row per attempt, in the order the nodes made them. Every execution of the plan consumes its own rows in order;
 * the scheduler decides when an execution is over, exactly as it did on the device.
 *
 * Output (stdout, one line per item) is read by tests/python/test_bench_ranging_replay.py:
 *   EXEC <index> <initiator> <reflector> attempts=<n> successes=<n> terminal=<0|1> rows_left=<n>
 *   PAIR <a> <b> state=<name> distance=<mm> confidence=<n> valid=<n> attempts=<n> rejected=<n> spread=<mm> disagreement=<mm> single=<0|1>
 *        diag_ab=<mm> diag_ba=<mm> n_ab=<n> n_ba=<n> ok_ab=<n> ok_ba=<n> cause=<name>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nrf_cs_scheduler.h"

#define MAX_ROWS 512U

typedef struct {
    unsigned exec;
    nrf_cs_outcome_t outcome;
    int32_t distance_mm;
    uint16_t confidence;
    unsigned used;
} row_t;

static row_t s_rows[MAX_ROWS];
static size_t s_row_count;

static const char *state_name(nrf_cs_pair_state_t state)
{
    switch (state) {
        case NRF_CS_PAIR_VALID: return "valid";
        case NRF_CS_PAIR_MISSING: return "missing";
        case NRF_CS_PAIR_RETRY_EXHAUSTED: return "retry_exhausted";
        case NRF_CS_PAIR_CABLE_EXCEEDED: return "cable_exceeded";
        case NRF_CS_PAIR_DIRECTIONS_DISAGREE: return "directions_disagree";
        case NRF_CS_PAIR_PENDING: return "pending";
        default: return "unknown";
    }
}

static int load(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[256];

    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned node, exec, attempt, dist, conf;
        char outcome[24];

        if (line[0] == '#' || strncmp(line, "node,", 5) == 0) continue;
        if (sscanf(line, "%u,%u,%u,%23[^,],%u,%u", &node, &exec, &attempt, outcome, &dist, &conf) != 6) continue;
        if (s_row_count >= MAX_ROWS) { fclose(file); return -1; }
        s_rows[s_row_count].exec = exec;
        s_rows[s_row_count].outcome = strcmp(outcome, "SUCCESS") == 0 ? NRF_CS_OUTCOME_SUCCESS
            : strcmp(outcome, "TIMEOUT") == 0 ? NRF_CS_OUTCOME_TIMEOUT
            : strcmp(outcome, "OUTLIER") == 0 ? NRF_CS_OUTCOME_OUTLIER : NRF_CS_OUTCOME_TRANSPORT_ERROR;
        s_rows[s_row_count].distance_mm = (int32_t)dist;
        s_rows[s_row_count].confidence = (uint16_t)conf;
        ++s_row_count;
    }
    fclose(file);
    return 0;
}

static row_t *next_row(unsigned exec)
{
    for (size_t i = 0U; i < s_row_count; ++i) {
        if (s_rows[i].exec == exec && !s_rows[i].used) {
            s_rows[i].used = 1U;
            return &s_rows[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static nrf_cs_scheduler_t scheduler;
    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 2400U, .max_attempts = 2U, .min_confidence = 500U, .samples_per_pair = 9U,
        .full_rotation = true, .relay_order = true, .require_both_directions = true,
    };
    const nrf_cs_node_id_t nodes[] = {0, 1, 3, 4};
    nrf_cs_step_t step;

    const char *csv = argc >= 2 ? argv[1] : getenv("BENCH_RANGING_CSV");
    const char *out_path = getenv("BENCH_RANGING_OUT");
    FILE *out = out_path != NULL ? fopen(out_path, "w") : stdout;

    if (csv == NULL || out == NULL || load(csv) != 0) return 2;
    if (nrf_cs_scheduler_init(&scheduler, &policy) != NRF_CS_OK ||
        nrf_cs_scheduler_start_cycle(&scheduler, nodes, 4U) != NRF_CS_OK) return 3;

    while (nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY) {
        row_t *row = next_row((unsigned)scheduler.active_execution_index);
        nrf_cs_measurement_t measurement = {0};

        if (row == NULL) {                      /* the device made no such attempt: stop and say where */
            printf("OUT_OF_ROWS exec=%u\n", (unsigned)scheduler.active_execution_index);
            return 4;
        }
        measurement.outcome = row->outcome;
        measurement.distance_mm = row->distance_mm;
        measurement.confidence = row->confidence;
        if (nrf_cs_scheduler_complete_step(&scheduler, &step, &measurement) != NRF_CS_OK) return 5;
    }
    for (size_t i = 0U; i < nrf_cs_scheduler_execution_count(&scheduler); ++i) {
        const nrf_cs_execution_t *e = &scheduler.executions[i];
        unsigned left = 0U;
        for (size_t r = 0U; r < s_row_count; ++r) left += (s_rows[r].exec == i && !s_rows[r].used) ? 1U : 0U;
        fprintf(out, "EXEC %u %u %u attempts=%u successes=%u terminal=%u rows_left=%u\n", (unsigned)i, (unsigned)e->initiator,
            (unsigned)e->reflector, (unsigned)e->attempts, (unsigned)e->successes, e->terminal ? 1U : 0U, left);
    }
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        const nrf_cs_pair_result_t *p = nrf_cs_scheduler_pair_at(&scheduler, i);
        fprintf(out, "PAIR %u %u state=%s distance=%d confidence=%u valid=%u attempts=%u rejected=%u spread=%d disagreement=%d single=%u "
               "diag_ab=%d diag_ba=%d n_ab=%u n_ba=%u ok_ab=%u ok_ba=%u cause=%s\n",
            (unsigned)p->node_a, (unsigned)p->node_b, state_name(p->state), (int)p->distance_mm, (unsigned)p->confidence,
            (unsigned)p->valid_samples, (unsigned)p->attempts, (unsigned)p->rejected_samples, (int)p->spread_mm,
            (int)p->direction_disagreement_mm, p->single_direction ? 1U : 0U, (int)p->diag_distance_mm[0],
            (int)p->diag_distance_mm[1], (unsigned)p->diag_samples[0], (unsigned)p->diag_samples[1],
            (unsigned)p->diag_accepted[0], (unsigned)p->diag_accepted[1], nrf_cs_pair_cause_name(nrf_cs_pair_cause(p, 3U)));
    }
    {
        nrf_cs_pair_result_t all[NRF_CS_MAX_PAIRS];
        char failed[96];
        const size_t count = nrf_cs_scheduler_pair_count(&scheduler);
        for (size_t i = 0U; i < count; ++i) all[i] = *nrf_cs_scheduler_pair_at(&scheduler, i);
        const size_t valid = nrf_cs_pairs_summary(all, count, 3U, failed, sizeof(failed));
        fprintf(out, "SUMMARY valid=%u/%u failed=%s\n", (unsigned)valid, (unsigned)count, failed[0] != 0 ? failed : "none");
        char tiny[12];
        (void)nrf_cs_pairs_summary(all, count, 3U, tiny, sizeof(tiny));
        fprintf(out, "SUMMARY_TINY %s\n", tiny[0] != 0 ? tiny : "none");
    }
    if (out != stdout) fclose(out);
    return 0;
}
