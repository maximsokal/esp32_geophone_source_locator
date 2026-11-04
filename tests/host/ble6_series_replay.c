/* Research of one direction only; rows are never relabelled as reciprocal measurements. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "nrf_cs_scheduler.h"

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U;
}

int main(void)
{
    const char *input = getenv("BLE6_SERIES_INPUT"), *output = getenv("BLE6_SERIES_OUTPUT");
    if (input == NULL || output == NULL) return 2;
    FILE *in = fopen(input, "rb"), *out = fopen(output, "w");
    if (in == NULL || out == NULL) return 2;
    uint8_t header[4];
    if (fread(header, 1U, sizeof(header), in) != sizeof(header)) return 3;
    const uint32_t count = read_u32(header);
    fputs("window,target,valid,distance_mm,spread_mm,rejected,attempts,samples,quality\n", out);
    for (uint32_t window = 0U; window < count; ++window) {
        uint8_t raw[60];
        if (fread(raw, 1U, sizeof(raw), in) != sizeof(raw)) return 3;
        for (uint8_t target = 3U; target <= 9U; target += 6U) {
            const nrf_cs_policy_t policy = {.pair_timeout_ms=2400U,.max_attempts=2U,
                .min_confidence=500U,.samples_per_pair=target};
            const nrf_cs_node_id_t nodes[] = {0U, 1U, 2U};
            nrf_cs_scheduler_t scheduler;
            assert(nrf_cs_scheduler_init(&scheduler, &policy) == NRF_CS_OK);
            assert(nrf_cs_scheduler_start_cycle(&scheduler, nodes, 3U) == NRF_CS_OK);
            unsigned used = 0U;
            while (nrf_cs_scheduler_pair_at(&scheduler, 0U)->state == NRF_CS_PAIR_PENDING) {
                nrf_cs_step_t step = {0};
                assert(used < 10U && nrf_cs_scheduler_next_step(&scheduler, &step) == NRF_CS_STEP_READY);
                assert(step.pair_index == 0U && step.initiator == 0U && step.reflector == 1U);
                const uint8_t *p = &raw[6U * used++];
                const uint32_t distance = read_u32(p);
                const uint16_t confidence = (uint16_t)((uint16_t)p[4] | (uint16_t)p[5] << 8U);
                if (confidence > 1000U) return 3;
                const nrf_cs_measurement_t m = {.outcome=distance <= INT32_MAX ? NRF_CS_OUTCOME_SUCCESS : NRF_CS_OUTCOME_OUTLIER,
                    .distance_mm=distance <= INT32_MAX ? (int32_t)distance : 0,.confidence=confidence};
                assert(nrf_cs_scheduler_complete_step(&scheduler, &step, &m) == NRF_CS_OK);
            }
            const nrf_cs_pair_result_t *p = nrf_cs_scheduler_pair_at(&scheduler, 0U);
            fprintf(out, "%u,%u,%u,%d,%d,%u,%u,%u,%u\n", window, (unsigned)target,
                p->state == NRF_CS_PAIR_VALID ? 1U : 0U, p->distance_mm, p->spread_mm,
                (unsigned)p->rejected_samples, (unsigned)p->attempts, (unsigned)p->valid_samples,
                (unsigned)p->scheduler_quality_flags);
        }
    }
    if (fgetc(in) != EOF) return 3;
    fclose(in);
    return fclose(out) == 0 ? 0 : 3;
}
