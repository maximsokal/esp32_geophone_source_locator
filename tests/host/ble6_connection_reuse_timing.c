/* Research latency model: no production connection reuse is implemented here. */
#include <math.h>
#include <stdlib.h>
#include "nrf_geometry_solver.h"
static uint32_t reuse_latency(const nrf_cs_step_t *step, unsigned execution);
static nrf_cs_measurement_t golden_measurement(unsigned execution,unsigned attempt);
#define NRF_CS_COORDINATOR_GOLDEN 1
#define NRF_CS_COORDINATOR_ATTEMPT_LATENCY(step, execution) reuse_latency(step, execution)
#define main existing_coordinator_suite_main
#include "nrf_cs_cycle_coordinator_test.c"
#undef main

static const int32_t points[5][2]={{0,0},{4000,0},{500,3500},{4300,3100},{1800,1600}};
static unsigned cold_ms,warm_ms,scenario,directed,handshakes;
static bool reuse_mode,ready[5];
static unsigned cached_execution[5];

static bool fault_at(unsigned execution,unsigned attempt)
{
    return scenario>=1U && scenario<=3U && execution==(directed>7U?7U:directed-1U) && attempt==3U;
}

static uint32_t reuse_latency(const nrf_cs_step_t *step,unsigned execution)
{
    const bool warm=reuse_mode && ready[step->initiator] && cached_execution[step->initiator]==execution;
    if(!warm)++handshakes;
    if(scenario==1U && fault_at(execution,step->attempt_number))return 6387U;
    return warm?warm_ms:cold_ms;
}

static nrf_cs_measurement_t golden_measurement(unsigned execution,unsigned attempt)
{
    const nrf_cs_cycle_step_t step=g_nodes[0].rt.steps[execution];
    nrf_cs_measurement_t m={.outcome=NRF_CS_OUTCOME_SUCCESS,.confidence=900U};
    const double dx=(double)points[step.initiator][0]-points[step.reflector][0];
    const double dy=(double)points[step.initiator][1]-points[step.reflector][1];
    m.distance_mm=(int32_t)lround(hypot(dx,dy));
    if(fault_at(execution,attempt)){
        if(scenario==3U)m.confidence=100U;
        else {m.outcome=scenario==1U?NRF_CS_OUTCOME_TIMEOUT:NRF_CS_OUTCOME_TRANSPORT_ERROR;m.distance_mm=0;m.confidence=0;}
    }
    ready[step.initiator]=m.outcome==NRF_CS_OUTCOME_SUCCESS && m.confidence>=500U;
    cached_execution[step.initiator]=execution;
    return m;
}

#define WIRE_CAPACITY (NRF_CS_MAX_NODES * (1U + (NRF_CS_CYCLE_MAX_RECORDS / NRF_CS_CYCLE_RESULTS_PER_PAGE + 1U) * NRF_CS_CYCLE_RESULT_LENGTH))
static size_t encode_records(const nrf_cs_cycle_collected_t *collected,uint32_t cycle,uint8_t *wire)
{
    size_t at=0U;
    for(size_t id=0;id<collected->node_count;++id){
        const nrf_cs_cycle_node_records_t *node=NULL;
        for(size_t i=0;i<collected->node_count;++i)if(collected->nodes[i].node==id)node=&collected->nodes[i];
        assert(node!=NULL && node->count>0U);
        wire[at++]=(uint8_t)id;
        const unsigned pages=(unsigned)((node->count+NRF_CS_CYCLE_RESULTS_PER_PAGE-1U)/NRF_CS_CYCLE_RESULTS_PER_PAGE);
        for(unsigned p=0;p<pages;++p){
            nrf_cs_cycle_result_t page={.request_id=1U,.cycle_id=cycle,.page=(uint8_t)p,
                .page_count=(uint8_t)pages,.record_total=(uint8_t)node->count};
            size_t offset=p*NRF_CS_CYCLE_RESULTS_PER_PAGE;
            size_t count=node->count-offset;
            if(count>NRF_CS_CYCLE_RESULTS_PER_PAGE)count=NRF_CS_CYCLE_RESULTS_PER_PAGE;
            page.record_count=(uint8_t)count;
            memcpy(page.records,node->records+offset,count*sizeof(page.records[0]));
            assert(at+NRF_CS_CYCLE_RESULT_LENGTH<=WIRE_CAPACITY);
            assert(nrf_cs_cycle_encode_result(&page,wire+at)==0);
            at+=NRF_CS_CYCLE_RESULT_LENGTH;
        }
    }
    return at;
}

int main(void)
{
    const char *input=getenv("BLE6_REUSE_PROFILES"),*output=getenv("BLE6_REUSE_OUTPUT");
    if(input==NULL||output==NULL)return 2;
    FILE *in=fopen(input,"r"),*out=fopen(output,"w");
    if(in==NULL||out==NULL)return 2;
    unsigned profiles;
    if(fscanf(in,"%u",&profiles)!=1||profiles==0U||profiles>3U)return 3;
    fputs("profile,nodes,rate,scenario,reuse,elapsed_ms,records,handshakes,accepted,budget_ms,wire_bytes\n",out);
    const uint32_t rates[]={25000U,50000U,100000U};
    uint8_t baseline[WIRE_CAPACITY],wire[WIRE_CAPACITY];
    for(unsigned profile=0;profile<profiles;++profile){
        if(fscanf(in,"%u %u",&cold_ms,&warm_ms)!=2||warm_ms==0U||warm_ms>cold_ms)return 3;
        for(size_t n=3;n<=5;++n)for(unsigned rate=0;rate<3;++rate)for(scenario=0;scenario<5;++scenario){
            size_t baseline_size=0U;
            for(unsigned mode=0;mode<2;++mode){
                nrf_cs_node_id_t ids[5];setup(n,ids);
                for(size_t i=0;i<n;++i){ids[i]=(uint8_t)i;g_nodes[i].id=ids[i];}
                g_main=0;g_bus_hz=rates[rate];g_relay_releases=scenario!=4U;
                reuse_mode=mode!=0U;directed=(unsigned)(n*(n-1U));handshakes=0U;
                memset(ready,0,sizeof(ready));
                const nrf_cs_policy_t policy={.pair_timeout_ms=2400U,.max_attempts=2U,.min_confidence=500U,
                    .samples_per_pair=9U,.full_rotation=true,.relay_order=true,.require_both_directions=true};
                nrf_cs_scheduler_t scheduler;
                start_scheduler(&scheduler,ids,n,&policy);
                nrf_cs_cycle_collected_t collected;nrf_cs_cycle_report_t report;
                const uint64_t start=g_now;
                assert(nrf_cs_cycle_coordinate(&g_ops,&scheduler,g_main,1000U,&collected,&report)==NRF_CS_COORD_OK);
                assert(report.collected_mask==(1U<<n)-1U && report.release_mask==(1U<<n)-1U);
                assert(collected.record_count==directed*9U+((scenario>=1U&&scenario<=3U)?1U:0U));
                const size_t size=encode_records(&collected,scheduler.cycle_id,wire);
                if(mode==0U){memcpy(baseline,wire,size);baseline_size=size;}
                else assert(size==baseline_size && memcmp(baseline,wire,size)==0);
                assert(nrf_cs_cycle_replay(&scheduler,collected.nodes,collected.node_count)==0);
                nrf_cs_pair_result_t pairs[10];nrf_geometry_node_input_t nodes[5];
                for(size_t i=0;i<n;++i)nodes[i]=(nrf_geometry_node_input_t){.node_id=(uint8_t)i};
                const size_t pair_count=nrf_cs_scheduler_pair_count(&scheduler);
                for(size_t i=0;i<pair_count;++i){pairs[i]=*nrf_cs_scheduler_pair_at(&scheduler,i);assert(pairs[i].state==NRF_CS_PAIR_VALID);}
                nrf_geometry_solution_t solution;
                assert(nrf_geometry_solver_solve_partial(nodes,n,pairs,pair_count,&solution)==NRF_GEOMETRY_OK);
                assert(nrf_geometry_metrics_consistent(solution.inconsistent_pair_count,solution.max_residual_mm));
                for(size_t i=0;i<n;++i)assert(solution.nodes[i].solved && nrf_geometry_position_uncertainty_acceptable(solution.nodes[i].uncertainty_mm));
                const uint32_t budget=nrf_cs_cycle_coordinator_budget_ms(&policy,directed,n);
                assert(g_now-start<=budget && budget<=600000U);
                fprintf(out,"%u,%zu,%u,%u,%u,%llu,%zu,%u,%u,%u,%zu\n",profile,n,rates[rate],scenario,mode,
                    (unsigned long long)(g_now-start),collected.record_count,handshakes,n>=4U?1U:0U,budget,size);
            }
        }
    }
    fclose(in);return fclose(out)==0?0:3;
}
