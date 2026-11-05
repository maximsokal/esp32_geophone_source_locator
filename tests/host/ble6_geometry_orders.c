/* Diagnostic subset/permutation search: inputs contain constraints, never true XY coordinates. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nrf_geometry_solver.h"
#include "nrf_geometry_triangles.h"
static nrf_geometry_triangle_report_t full_triangles;

static unsigned bits(unsigned mask)
{
    unsigned result=0;for(;mask;mask>>=1U)result+=mask&1U;return result;
}

static bool next_order(unsigned *order, unsigned count)
{
    unsigned i=count-1U;
    while(i>0U && order[i-1U]>=order[i])--i;
    if(i==0U)return false;
    unsigned j=count-1U;
    while(order[j]<=order[i-1U])--j;
    unsigned tmp=order[i-1U];order[i-1U]=order[j];order[j]=tmp;
    for(j=count-1U;i<j;++i,--j){tmp=order[i];order[i]=order[j];order[j]=tmp;}
    return true;
}

static const nrf_geometry_node_position_t *node(const nrf_geometry_solution_t *s,unsigned id)
{
    for(size_t i=0;i<s->node_count;++i)if(s->nodes[i].node_id==id)return &s->nodes[i];
    return NULL;
}

static double distance(const nrf_geometry_solution_t *s,unsigned a,unsigned b)
{
    const nrf_geometry_node_position_t *x=node(s,a),*y=node(s,b);
    if(x==NULL||y==NULL)return NAN;
    const double dx=(double)x->x_mm-y->x_mm,dy=(double)x->y_mm-y->y_mm,dz=(double)x->z_mm-y->z_mm;
    return sqrt(dx*dx+dy*dy+dz*dz);
}

static bool qualify(const nrf_geometry_node_input_t *inputs,size_t n,
    const nrf_cs_pair_result_t *pairs,size_t count,const uint32_t *cables,nrf_geometry_solution_t *s)
{
    nrf_cs_pair_result_t valid[NRF_CS_MAX_PAIRS];
    bool partial=false;
    memcpy(valid,pairs,count*sizeof(*valid));
    for(size_t p=0;p<count;++p)if(valid[p].state!=NRF_CS_PAIR_VALID){valid[p].state=NRF_CS_PAIR_UNKNOWN;partial=true;}
    if(partial)for(size_t i=0;i<n;++i){
        unsigned degree=0;
        for(size_t p=0;p<count;++p)if(valid[p].state==NRF_CS_PAIR_VALID &&
            (valid[p].node_a==inputs[i].node_id||valid[p].node_b==inputs[i].node_id))++degree;
        if(degree<3U)return false;
    }
    nrf_geometry_triangle_report_t triangle;
    if(!nrf_geometry_triangles_check(valid,count,&triangle)||triangle.inconsistent_count)return false;
    if(nrf_geometry_solver_solve_partial(inputs,n,valid,count,s)!=NRF_GEOMETRY_OK ||
        !nrf_geometry_metrics_consistent(s->inconsistent_pair_count,s->max_residual_mm))return false;
    for(size_t i=0;i<n;++i){
        const nrf_geometry_node_position_t *p=&s->nodes[i];
        if(!p->solved||!nrf_geometry_position_uncertainty_acceptable(p->uncertainty_mm))return false;
        if(p->node_id!=0U && distance(s,0U,p->node_id)>(double)cables[p->node_id]+1.0)return false;
    }
    return true;
}

static void print_solution(FILE *out,unsigned case_id,unsigned mask,unsigned order,bool ok,
    const nrf_geometry_solution_t *s,bool baseline)
{
    fprintf(out,"%u,%u,%u,%u,%u,%d",case_id,mask,order,ok?1U:0U,baseline?1U:0U,s->max_residual_mm);
    for(unsigned id=0;id<5U;++id){
        const nrf_geometry_node_position_t *p=node(s,id);
        fprintf(out,",%d,%d,%d,%d",p?p->x_mm:0,p?p->y_mm:0,p?p->z_mm:0,p?p->uncertainty_mm:INT32_MAX);
    }
    for(size_t p=0;p<NRF_CS_MAX_PAIRS;++p)fprintf(out,",%u",(unsigned)full_triangles.pair_violations[p]);
    fputc('\n',out);
}

int main(void)
{
    const char *input=getenv("BLE6_GEOMETRY_INPUT"),*output=getenv("BLE6_GEOMETRY_OUTPUT");
    if(input==NULL||output==NULL)return 2;
    FILE *in=fopen(input,"r"),*out=fopen(output,"w");
    if(in==NULL||out==NULL)return 2;
    unsigned cases;
    if(fscanf(in,"%u",&cases)!=1)return 3;
    for(unsigned case_id=0;case_id<cases;++case_id){
        unsigned n;
        if(fscanf(in,"%u",&n)!=1||n<4U||n>5U)return 3;
        int z[5];uint32_t cables[5];
        for(unsigned i=0;i<5U;++i)if(fscanf(in,"%d %u",&z[i],&cables[i])!=2)return 3;
        nrf_cs_pair_result_t pairs[10]={0};
        for(unsigned p=0;p<10U;++p){
            int d,spread;unsigned flags,state;
            if(fscanf(in,"%d %d %u %u",&d,&spread,&flags,&state)!=4||spread<0||flags>UINT16_MAX||state>NRF_CS_PAIR_DIRECTIONS_DISAGREE)return 3;
            pairs[p].distance_mm=d;pairs[p].spread_mm=spread;pairs[p].confidence=900U;
            pairs[p].scheduler_quality_flags=(uint16_t)flags;pairs[p].state=(nrf_cs_pair_state_t)state;
        }
        unsigned count=0;
        for(unsigned a=0;a<n;++a)for(unsigned b=a+1;b<n;++b){
            pairs[count].node_a=(uint8_t)a;pairs[count].node_b=(uint8_t)b;
            if(a==0U&&pairs[count].state==NRF_CS_PAIR_VALID&&pairs[count].distance_mm>(int32_t)cables[b])pairs[count].state=NRF_CS_PAIR_CABLE_EXCEEDED;
            ++count;
        }
        unsigned permutation[4]={1,2,3,4},order=0;
        if(!nrf_geometry_triangles_check(pairs,count,&full_triangles))return 3;
        do{
            nrf_geometry_node_input_t inputs[5]={{.node_id=0U,.z_mm=z[0]}};
            for(unsigned i=1;i<n;++i)inputs[i]=(nrf_geometry_node_input_t){.node_id=(uint8_t)permutation[i-1U],.z_mm=z[permutation[i-1U]]};
            for(unsigned mask=0;mask<(1U<<count);++mask){
                if(bits(mask)>2U)continue;
                bool allowed=true;
                nrf_cs_pair_result_t subset[10];memcpy(subset,pairs,sizeof(subset));
                for(unsigned p=0;p<count;++p)if(mask&(1U<<p)){
                    if(pairs[p].state!=NRF_CS_PAIR_VALID||(pairs[p].scheduler_quality_flags&NRF_CS_QUALITY_MANUAL))allowed=false;
                    subset[p].state=NRF_CS_PAIR_UNKNOWN;
                }
                if(!allowed)continue;
                nrf_geometry_solution_t solution={0};
                bool ok=qualify(inputs,n,subset,count,cables,&solution);
                if(ok)for(unsigned p=0;p<count;++p)if(mask&(1U<<p)){
                    if(fabs(distance(&solution,pairs[p].node_a,pairs[p].node_b)-pairs[p].distance_mm)<=NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM)ok=false;
                }
                if(mask==0U&&order==0U)print_solution(out,case_id,mask,order,ok,&solution,true);
                if(ok)print_solution(out,case_id,mask,order,true,&solution,false);
            }
            ++order;
        }while(next_order(permutation,n-1U));
    }
    fclose(in);return fclose(out)==0?0:3;
}
