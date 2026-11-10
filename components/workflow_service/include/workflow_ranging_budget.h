#pragma once

#include <stddef.h>
#include <stdint.h>


/*
 * The time one Channel Sounding attempt may take (pair_timeout_ms of the ranging policy).
 *
 * Every attempt sets the link up from nothing: connect, encrypt, MTU, discovery, features, capabilities, config, CS
 * security, and only then the procedure. A node log of 2026-11-10 17:52 shows the working attempts at 1.6-1.9 s, one
 * that ran out of the 2.4 s budget in PROCEDURE because the connect alone took 1.08 s, and one that waited 4 s for
 * the disconnect. Two failed attempts end a direction, a pair needs both directions, so with 2.4 s a three-node bench
 * produced no valid pair at all.
 *
 * The limit on the other side is the I2C bus: the ESP32 keeps it silent for the whole cycle and
 * sensor_bus_quiet_begin() refuses more than 600 s. The cycle length is the worst case, executions * window, and it
 * grows with the square of the node count. The base budget is what MAIN + 4 nodes (20 executions) can afford; a
 * smaller network gets the relaxed budget whenever its worst case still fits the limit.
 */
#define WORKFLOW_RANGING_PAIR_BUDGET_MS 2400U
#define WORKFLOW_RANGING_PAIR_BUDGET_RELAXED_MS 4000U
#define WORKFLOW_RANGING_BUS_QUIET_LIMIT_MS 600000U

/*
 * The terms of nrf_cs_cycle_coordinator_budget_ms(), repeated here so this header needs nothing from the node
 * firmware tree. workflow_ranging_budget_test pins every one of them to the shared definition.
 */
#define WORKFLOW_RANGING_GO_SKEW_MS 300U
#define WORKFLOW_RANGING_ATTEMPT_GAP_MS 150U
#define WORKFLOW_RANGING_TOTAL_MARGIN_MS 3000U
#define WORKFLOW_RANGING_COLLECT_GRACE_MS 6000U
#define WORKFLOW_RANGING_NODE_OVERHEAD_MS 2500U

/* One execution: the GO skew, then samples_per_pair + max_attempts - 1 attempts, each with its gap. */
static inline uint64_t workflow_ranging_window_ms(
    uint16_t pair_timeout_ms, uint8_t samples_per_pair, uint8_t max_attempts)
{
    const uint64_t attempts = (uint64_t)samples_per_pair + max_attempts - 1U;
    return WORKFLOW_RANGING_GO_SKEW_MS +
        attempts * ((uint64_t)pair_timeout_ms + WORKFLOW_RANGING_ATTEMPT_GAP_MS);
}

/* Worst-case cycle length. */
static inline uint64_t workflow_ranging_cycle_ms(
    size_t node_count, uint16_t pair_timeout_ms, uint8_t samples_per_pair, uint8_t max_attempts)
{
    const uint64_t executions = node_count < 2U ? 0U : (uint64_t)node_count * (node_count - 1U);
    return executions * workflow_ranging_window_ms(pair_timeout_ms, samples_per_pair, max_attempts) +
        WORKFLOW_RANGING_TOTAL_MARGIN_MS + WORKFLOW_RANGING_COLLECT_GRACE_MS +
        (uint64_t)node_count * WORKFLOW_RANGING_NODE_OVERHEAD_MS;
}

/*
 * node_count is every node that may take part. The caller counts MAIN even when it is not there, so the choice
 * never makes a cycle that cannot start.
 */
static inline uint16_t workflow_ranging_pair_budget_ms(
    size_t node_count, uint8_t samples_per_pair, uint8_t max_attempts)
{
    return workflow_ranging_cycle_ms(
            node_count, WORKFLOW_RANGING_PAIR_BUDGET_RELAXED_MS, samples_per_pair, max_attempts) <=
        WORKFLOW_RANGING_BUS_QUIET_LIMIT_MS
        ? WORKFLOW_RANGING_PAIR_BUDGET_RELAXED_MS
        : WORKFLOW_RANGING_PAIR_BUDGET_MS;
}
