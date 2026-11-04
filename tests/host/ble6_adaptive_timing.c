#include <math.h>
#include <stdlib.h>
#include "nrf_cs_scheduler.h"
#include "nrf_geometry_solver.h"
static nrf_cs_measurement_t golden_measurement(unsigned execution, unsigned attempt);
#define NRF_CS_COORDINATOR_GOLDEN 1
#define main existing_coordinator_main
#include "nrf_cs_cycle_coordinator_test.c"
#undef main

static const int32_t points[5][2] = {{0,0},{4000,0},{500,3500},{4300,3100},{1800,1600}};

static nrf_cs_measurement_t golden_measurement(unsigned execution, unsigned attempt)
{
    (void)attempt;
    const nrf_cs_cycle_step_t step = g_nodes[0].rt.steps[execution];
    const double dx = (double)points[step.initiator][0] - points[step.reflector][0];
    const double dy = (double)points[step.initiator][1] - points[step.reflector][1];
    return (nrf_cs_measurement_t){.outcome=NRF_CS_OUTCOME_SUCCESS,
        .distance_mm=(int32_t)(hypot(dx,dy)+.5),.confidence=900U};
}

int main(void)
{
    const char *path = getenv("BLE6_TIMING_OUTPUT");
    if (path == NULL) return 2;
    FILE *out = fopen(path, "w");
    if (out == NULL) return 2;
    fputs("mode,elapsed_ms,procedures,budget_sum_ms\n",out);
    const char *names[] = {"9","3","3+9","3+6"};
    const uint8_t targets[][2] = {{9,0},{3,0},{3,9},{3,6}};
    for (size_t mode=0;mode<4;++mode) {
        nrf_cs_node_id_t ids[5]; setup(5U,ids);
        for (size_t i=0;i<5;++i) {ids[i]=(uint8_t)i;g_nodes[i].id=ids[i];}
        g_main=0;g_ble_latency_ms=1300;g_bus_hz=100000;g_relay_releases=true;
        const nrf_cs_policy_t policy = {.pair_timeout_ms=2400,.max_attempts=2,.min_confidence=500,
            .samples_per_pair=9,.full_rotation=true,.relay_order=true,.require_both_directions=true};
        nrf_cs_scheduler_t s;
        assert(nrf_cs_scheduler_init(&s,&policy)==NRF_CS_OK);
        const uint64_t started=g_now;
        unsigned procedures=0;
        uint32_t budget_sum=0U;
        for (unsigned pass=0;pass<2 && targets[mode][pass];++pass) {
            s.policy.samples_per_pair=targets[mode][pass];
            budget_sum+=nrf_cs_cycle_coordinator_budget_ms(&s.policy,20U,5U);
            assert(nrf_cs_scheduler_start_cycle(&s,ids,5U)==NRF_CS_OK);
            nrf_cs_cycle_collected_t collected; nrf_cs_cycle_report_t report;
            assert(nrf_cs_cycle_coordinate(&g_ops,&s,g_main,1000U+pass*100U,&collected,&report)==NRF_CS_COORD_OK);
            assert(report.collected_mask==31U && report.release_mask==31U);
            assert(nrf_cs_cycle_replay(&s,collected.nodes,collected.node_count)==0);
            procedures+=(unsigned)collected.record_count;
            nrf_cs_pair_result_t pairs[10];
            for(size_t i=0;i<10;++i){pairs[i]=*nrf_cs_scheduler_pair_at(&s,i);assert(pairs[i].state==NRF_CS_PAIR_VALID);}
            nrf_geometry_node_input_t input[5];
            for(size_t i=0;i<5;++i)input[i]=(nrf_geometry_node_input_t){.node_id=(uint8_t)i,.z_mm=0};
            nrf_geometry_solution_t solution;
            assert(nrf_geometry_solver_solve(input,5U,pairs,10U,&solution)==NRF_GEOMETRY_OK);
            assert(nrf_geometry_metrics_consistent(solution.inconsistent_pair_count,solution.max_residual_mm));
            for(size_t i=0;i<5;++i)assert(solution.nodes[i].solved &&
                nrf_geometry_position_uncertainty_acceptable(solution.nodes[i].uncertainty_mm));
            assert(g_now-started<600000U);
        }
        fprintf(out,"%s,%llu,%u,%u\n",names[mode],(unsigned long long)(g_now-started),procedures,budget_sum);
    }
    return fclose(out)==0?0:3;
}
