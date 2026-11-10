#include <assert.h>
#include <stdint.h>

#include "nrf_cs_cycle_coordinator.h"
#include "nrf_cs_cycle_runtime.h"
#include "workflow_ranging_budget.h"

/* The policy of the workflow: nine samples per direction and one spare try. */
#define SAMPLES 9U
#define ATTEMPTS 2U

int main(void)
{
    /* The workflow's copy of the budget terms is the node firmware's: a change on either side fails here. */
    assert(WORKFLOW_RANGING_GO_SKEW_MS == NRF_CS_CYCLE_GO_SKEW_MS);
    assert(WORKFLOW_RANGING_ATTEMPT_GAP_MS == NRF_CS_CYCLE_ATTEMPT_GAP_MS);
    assert(WORKFLOW_RANGING_TOTAL_MARGIN_MS == NRF_CS_CYCLE_TOTAL_MARGIN_MS);
    assert(WORKFLOW_RANGING_COLLECT_GRACE_MS == NRF_CS_COORD_COLLECT_GRACE_MS);
    assert(WORKFLOW_RANGING_NODE_OVERHEAD_MS == NRF_CS_COORD_NODE_OVERHEAD_MS);
    for (unsigned budget = 1000U; budget <= 6000U; budget += 500U) {
        for (unsigned samples = 1U; samples <= 12U; ++samples) {
            assert(workflow_ranging_window_ms((uint16_t)budget, (uint8_t)samples, ATTEMPTS) ==
                nrf_cs_cycle_window_ms((uint16_t)budget, (uint8_t)samples, ATTEMPTS));
        }
    }
    assert(workflow_ranging_window_ms(2400U, SAMPLES, ATTEMPTS) == 25800U);
    assert(workflow_ranging_window_ms(4000U, SAMPLES, ATTEMPTS) == 41800U);
    /* MAIN + 4 nodes: 20 executions. 2.4 s fits the 600 s bus-quiet limit, 4 s does not. */
    assert(workflow_ranging_cycle_ms(5U, 2400U, SAMPLES, ATTEMPTS) == 537500U);
    assert(workflow_ranging_cycle_ms(5U, 4000U, SAMPLES, ATTEMPTS) == 857500U);
    assert(workflow_ranging_cycle_ms(5U, 2400U, SAMPLES, ATTEMPTS) <= WORKFLOW_RANGING_BUS_QUIET_LIMIT_MS);

    /* The bench of 2026-11-10: three nodes, counted as four because MAIN may be there. 4 s fits with room. */
    assert(workflow_ranging_cycle_ms(4U, 4000U, SAMPLES, ATTEMPTS) == 520600U);
    assert(workflow_ranging_pair_budget_ms(3U, SAMPLES, ATTEMPTS) == 4000U);
    assert(workflow_ranging_pair_budget_ms(4U, SAMPLES, ATTEMPTS) == 4000U);
    /* The production network keeps the budget it can afford: a longer one would make the cycle unstartable. */
    assert(workflow_ranging_pair_budget_ms(5U, SAMPLES, ATTEMPTS) == 2400U);
    assert(workflow_ranging_pair_budget_ms(9U, SAMPLES, ATTEMPTS) == 2400U);

    /* Whatever is chosen always fits the limit for the node counts the hardware has. */
    for (size_t nodes = 2U; nodes <= 5U; ++nodes) {
        const uint16_t budget = workflow_ranging_pair_budget_ms(nodes, SAMPLES, ATTEMPTS);
        assert(workflow_ranging_cycle_ms(nodes, budget, SAMPLES, ATTEMPTS) <= WORKFLOW_RANGING_BUS_QUIET_LIMIT_MS);
    }
    /* More samples per direction cost time too: the relaxed budget is given up as soon as it stops fitting. */
    assert(workflow_ranging_pair_budget_ms(4U, 12U, ATTEMPTS) == 2400U);
    return 0;
}
