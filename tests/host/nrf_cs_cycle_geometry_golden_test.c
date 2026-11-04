#include <math.h>
#include "nrf_geometry_solver.h"
static nrf_cs_measurement_t golden_measurement(unsigned execution,unsigned attempt);
#define NRF_CS_COORDINATOR_GOLDEN 1
#define main existing_coordinator_suite_main
#include "nrf_cs_cycle_coordinator_test.c"
#undef main

static int32_t coords[5][3];
static bool all_timeout;

static int32_t oracle_distance(unsigned a,unsigned b)
{
    double sum=0;
    for(unsigned k=0;k<3;++k) {
        double difference=(double)coords[a][k]-coords[b][k];
        sum+=difference*difference;
    }
    return (int32_t)(sqrt(sum)+0.5);
}

static nrf_cs_measurement_t golden_measurement(unsigned execution,unsigned attempt)
{
    (void)attempt;
    const nrf_cs_cycle_step_t step=g_nodes[0].rt.steps[execution];
    if(all_timeout)return (nrf_cs_measurement_t){.outcome=NRF_CS_OUTCOME_TIMEOUT};
    return (nrf_cs_measurement_t){.outcome=NRF_CS_OUTCOME_SUCCESS,
        .distance_mm=oracle_distance(step.initiator,step.reflector),.confidence=900};
}

static uint32_t scenario(size_t count,uint32_t rate,bool fail,const nrf_cs_policy_t *policy,
    uint32_t latency_ms,bool releases)
{
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t result;
    nrf_cs_cycle_report_t report;
    nrf_geometry_node_input_t inputs[5];
    nrf_geometry_solution_t solution;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    const int32_t square[5][3]={{0,0,0},{1000,0,0},{0,1000,0},{1000,1000,0},{400,500,600}};
    memcpy(coords,square,sizeof(coords));
    if(count==5){coords[1][2]=200;coords[2][2]=-100;coords[3][2]=300;}
    setup(count,ids);
    for(size_t i=0;i<count;++i){ids[i]=(nrf_cs_node_id_t)i;g_nodes[i].id=ids[i];}
    g_main=0;g_bus_hz=rate;g_ble_latency_ms=latency_ms;g_relay_releases=releases;all_timeout=fail;
    start_scheduler(&scheduler,ids,count,policy);
    const uint64_t started=g_now;
    assert(nrf_cs_cycle_coordinate(&g_ops,&scheduler,g_main,500,&result,&report)==NRF_CS_COORD_OK);
    assert(report.collected_mask==(1U<<count)-1U);
    assert(report.release_mask==(1U<<count)-1U);
    assert(nrf_cs_cycle_replay(&scheduler,result.nodes,result.node_count)==0);
    assert(nrf_cs_scheduler_pair_count(&scheduler)==count*(count-1)/2);
    unsigned directed[NRF_CS_CYCLE_MAX_STEPS]={0};
    uint32_t digest=2166136261U;
    for(size_t n=0;n<count;++n){
        assert(!nrf_cs_cycle_runtime_reply_active(&g_nodes[n].rt));
        inputs[n]=(nrf_geometry_node_input_t){.node_id=ids[n],.z_mm=coords[ids[n]][2]};
        for(size_t j=0;j<result.nodes[n].count;++j){
            const nrf_cs_cycle_record_t *record=&result.nodes[n].records[j];
            assert(record->execution<count*(count-1));
            assert(scheduler.executions[record->execution].initiator==result.nodes[n].node);
            ++directed[record->execution];
            digest=(digest^(uint32_t)record->distance_mm)*16777619U;
            digest=(digest^record->outcome)*16777619U;
        }
    }
    const unsigned expected_records=fail?policy->max_attempts:policy->samples_per_pair;
    for(size_t e=0;e<count*(count-1);++e)assert(directed[e]==expected_records);
    for(size_t i=0;i<nrf_cs_scheduler_pair_count(&scheduler);++i){
        pairs[i]=*nrf_cs_scheduler_pair_at(&scheduler,i);
        if(!fail){
            assert(pairs[i].state==NRF_CS_PAIR_VALID);
            assert(pairs[i].distance_mm==oracle_distance(pairs[i].node_a,pairs[i].node_b));
        }else assert(pairs[i].state!=NRF_CS_PAIR_VALID);
    }
    assert(nrf_geometry_solver_solve(inputs,count,pairs,nrf_cs_scheduler_pair_count(&scheduler),&solution)==NRF_GEOMETRY_OK);
    assert(solution.node_count==count);
    for(size_t i=0;i<count;++i)assert(solution.nodes[i].solved==(!fail || solution.nodes[i].node_id==0));
    if(!fail){
        assert(solution.inconsistent_pair_count==0 && solution.max_residual_mm<=5);
        for(size_t i=0;i<count;++i)assert(nrf_geometry_position_uncertainty_acceptable(solution.nodes[i].uncertainty_mm));
        for(size_t a=0;a<count;++a)for(size_t b=a+1;b<count;++b){
            double dx=solution.nodes[a].x_mm-solution.nodes[b].x_mm;
            double dy=solution.nodes[a].y_mm-solution.nodes[b].y_mm;
            double dz=solution.nodes[a].z_mm-solution.nodes[b].z_mm;
            assert(fabs(sqrt(dx*dx+dy*dy+dz*dz)-oracle_distance(solution.nodes[a].node_id,solution.nodes[b].node_id))<=5.0);
        }
    }
    if(releases)assert(report.relay_early);
    const uint64_t quiet_end=report.relay_early?g_main_complete_seen_ms:last_go_end()+report.run_ms;
    assert(count_ops(last_go_end()+1,quiet_end,true,0)==0);
    assert(g_now-started<=nrf_cs_cycle_coordinator_budget_ms(policy,count*(count-1),count));
    printf("golden nodes=%u rate=%u samples=%u releases=%u directed=%u pairs=%u records=%zu elapsed_ms=%llu all_timeout=%u digest=%u\n",
        (unsigned)count,rate,policy->samples_per_pair,releases?1U:0U,(unsigned)(count*(count-1)),
        (unsigned)nrf_cs_scheduler_pair_count(&scheduler),result.record_count,(unsigned long long)(g_now-started),fail?1U:0U,digest);
    return digest;
}

int main(void)
{
    const uint32_t rates[]={25000,50000,100000};
    for(size_t n=4;n<=5;++n)for(unsigned r=0;r<3;++r){
        uint32_t first=scenario(n,rates[r],false,&k_policy,100U,false);
        assert(scenario(n,rates[r],false,&k_policy,100U,false)==first);
        (void)scenario(n,rates[r],true,&k_policy,100U,false);
    }
    const nrf_cs_policy_t workflow={.pair_timeout_ms=2400U,.max_attempts=2U,.min_confidence=500U,
        .samples_per_pair=9U,.full_rotation=true,.relay_order=true,.require_both_directions=true};
    for(unsigned r=0;r<3;++r){
        const uint32_t first=scenario(5U,rates[r],false,&workflow,1300U,false);
        assert(scenario(5U,rates[r],false,&workflow,1300U,true)==first);
    }
    return 0;
}
