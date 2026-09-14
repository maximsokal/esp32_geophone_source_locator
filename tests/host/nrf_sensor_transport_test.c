#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nrf_sensor_transport.h"

typedef enum { M_OK, M_FOREIGN, M_PENDING, M_NACK, M_PROTOCOL } mode_t;
typedef struct {
    int sel; uint8_t ch; uint64_t now; uint32_t req; mode_t mode;
    unsigned select, close, main_reads, remote_reads, snaps, writes, discovery;
    bool main_absent, main_bad_epoch, fail_select, fail_close, mux_fault, abort;
} bus_t;

static void node(nrf_topology_node_snapshot_t *n, uint8_t role, uint8_t ch, uint8_t hw, uint32_t epoch) {
    memset(n,0,sizeof(*n)); n->state=NRF_TOPOLOGY_NODE_ASSIGNED; n->hardware_id_length=4;
    for(unsigned i=0;i<4;i++) n->hardware_id[i]=(uint8_t)(hw+i);
    n->node_id=role==NRF_UPSTREAM_ROLE_MAIN?0U:(uint8_t)(ch+1U);
    n->mux_channel=role==NRF_UPSTREAM_ROLE_MAIN?NRF_UPSTREAM_MUX_CHANNEL_NONE:ch;
    n->current_address=role==NRF_UPSTREAM_ROLE_MAIN?NRF_UPSTREAM_ADDRESS_MAIN:NRF_UPSTREAM_ADDRESS_REMOTE;
    n->protocol_result=NRF_UPSTREAM_RESULT_OK; n->assignment_request_id=epoch;
}
static void status(nrf_upstream_status_t *s,const nrf_topology_node_snapshot_t*n,uint8_t role){
    memset(s,0,sizeof(*s)); s->request_id=n->assignment_request_id; s->hardware_id_length=n->hardware_id_length;
    memcpy(s->hardware_id,n->hardware_id,sizeof(s->hardware_id)); s->role=role; s->node_id=n->node_id;
    s->mux_channel=n->mux_channel; s->target_address=n->current_address; s->lifecycle=NRF_UPSTREAM_LIFECYCLE_READY;
    s->result=NRF_UPSTREAM_RESULT_OK; s->flags=NRF_UPSTREAM_STATUS_FLAG_ASSIGNED|NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
}
static void topo(bus_t*b,nrf_topology_snapshot_t*t,uint8_t ch){
    memset(b,0,sizeof(*b)); memset(t,0,sizeof(*t)); b->sel=-1; b->ch=ch; t->generation=1; t->tca_available=true;
    node(&t->main,NRF_UPSTREAM_ROLE_MAIN,0,0x10,0x1111);
    for(unsigned i=0;i<NRF_TOPOLOGY_REMOTE_COUNT;i++) t->remotes[i].state=NRF_TOPOLOGY_NODE_ABSENT;
    node(&t->remotes[ch],NRF_UPSTREAM_ROLE_REMOTE,ch,(uint8_t)(0x30+ch),(uint32_t)(0x2200+ch));
}
static esp_err_t sel(void*c,uint8_t ch){bus_t*b=c;b->select++;assert(ch==b->ch);if(b->fail_select)return ESP_FAIL;b->sel=ch;return ESP_OK;}
static esp_err_t close_mux(void*c){bus_t*b=c;b->close++;b->sel=-1;return b->fail_close?ESP_FAIL:ESP_OK;}
static bool fault(void*c){return ((bus_t*)c)->mux_fault;}
static esp_err_t rd(void*c,uint8_t a,nrf_upstream_status_t*s){
    bus_t*b=c; nrf_topology_node_snapshot_t n;
    if(a==NRF_UPSTREAM_ADDRESS_DISCOVERY){b->discovery++;return ESP_FAIL;}
    if(a==NRF_UPSTREAM_ADDRESS_MAIN){b->main_reads++; if(b->main_absent)return ESP_ERR_NOT_FOUND;
        node(&n,NRF_UPSTREAM_ROLE_MAIN,0,0x10,b->main_bad_epoch?0x9999:0x1111); status(s,&n,NRF_UPSTREAM_ROLE_MAIN);return ESP_OK;}
    if(a==NRF_UPSTREAM_ADDRESS_REMOTE){b->remote_reads++; if(b->mode==M_NACK)return ESP_ERR_NOT_FOUND;
        node(&n,NRF_UPSTREAM_ROLE_REMOTE,b->ch,(uint8_t)(0x30+b->ch),(uint32_t)(0x2200+b->ch));status(s,&n,NRF_UPSTREAM_ROLE_REMOTE);return ESP_OK;}
    return ESP_ERR_INVALID_ARG;
}
static esp_err_t wr(void*c,const nrf_upstream_sensor_refresh_t*r){bus_t*b=c;b->writes++;b->req=r->request_id;return ESP_OK;}
static esp_err_t snap(void*c,nrf_upstream_status_t*s,nrf_upstream_sensor_snapshot_t*x){
    bus_t*b=c; nrf_topology_node_snapshot_t n; b->snaps++; if(b->mode==M_PROTOCOL)return ESP_ERR_INVALID_RESPONSE;
    node(&n,NRF_UPSTREAM_ROLE_REMOTE,b->ch,(uint8_t)(0x30+b->ch),(uint32_t)(0x2200+b->ch));status(s,&n,NRF_UPSTREAM_ROLE_REMOTE);
    memset(x,0,sizeof(*x));x->sequence=42;x->sample_complete_uptime_ms=1234;
    if(b->mode==M_FOREIGN && b->writes==1){x->request_id=0;x->result=b->snaps==1?NRF_UPSTREAM_SENSOR_RESULT_PENDING:NRF_UPSTREAM_SENSOR_RESULT_OK;}
    else {x->request_id=b->req;x->result=b->mode==M_PENDING?NRF_UPSTREAM_SENSOR_RESULT_PENDING:NRF_UPSTREAM_SENSOR_RESULT_OK;x->valid_mask=NRF_UPSTREAM_SENSOR_VALID_MASK;x->pressure_pa=101325;}
    return ESP_OK;
}
static void sleep10(void*c,uint32_t ms){bus_t*b=c;assert(ms==10);b->now+=ms;}
static uint64_t now(void*c){return ((bus_t*)c)->now;}
static bool aborted(void*c){return ((bus_t*)c)->abort;}
static nrf_sensor_transport_io_t io(bus_t*b){return (nrf_sensor_transport_io_t){b,sel,close_mux,fault,rd,wr,snap,sleep10,now,aborted};}
static nrf_sensor_transport_controller_t ctl(void){nrf_sensor_transport_controller_t c; nrf_sensor_transport_config_t cfg={500,10,77};assert(nrf_sensor_transport_controller_init(&c,&cfg)==ESP_OK);return c;}
static void cache(const nrf_topology_snapshot_t*t,nrf_remote_sensor_cache_t*c,uint8_t ch){memset(c,0,sizeof(*c)*NRF_TOPOLOGY_REMOTE_COUNT);assert(nrf_sensor_cache_reconcile_topology(t,c)==(1U<<ch));}

static void test_cache_epoch(void){
    bus_t b; nrf_topology_snapshot_t t; nrf_remote_sensor_cache_t c[NRF_TOPOLOGY_REMOTE_COUNT]={0}; topo(&b,&t,3);assert(nrf_sensor_cache_reconcile_topology(&t,c)==8);
    c[3].response_valid=true;c[3].state=NRF_SENSOR_CACHE_OK;c[3].last_attempt_state=NRF_SENSOR_CACHE_OK;c[3].sensor.sequence=9;t.generation++;
    assert(nrf_sensor_cache_reconcile_topology(&t,c)==0&&c[3].response_valid&&c[3].sensor.sequence==9);
    t.remotes[3].state=NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;assert(nrf_sensor_cache_reconcile_topology(&t,c)==0&&!c[3].topology_current&&c[3].response_valid);
    t.remotes[3].state=NRF_TOPOLOGY_NODE_ASSIGNED;assert(nrf_sensor_cache_reconcile_topology(&t,c)==0&&c[3].state==NRF_SENSOR_CACHE_OK);
    t.remotes[3].assignment_request_id++;assert(nrf_sensor_cache_reconcile_topology(&t,c)==8&&!c[3].response_valid&&c[3].state==NRF_SENSOR_CACHE_EMPTY);
}
static void test_success_and_no_discovery(void){
    for(uint8_t ch=0;ch<8;ch++){bus_t b;nrf_topology_snapshot_t t;nrf_remote_sensor_cache_t c[8];topo(&b,&t,ch);cache(&t,c,ch);nrf_sensor_transport_controller_t k=ctl();nrf_sensor_transport_io_t i=io(&b);bool r=false;
        assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,ch,&c[ch],&r)==ESP_OK);assert(c[ch].state==NRF_SENSOR_CACHE_OK&&c[ch].sensor.pressure_pa==101325&&c[ch].esp_receipt_uptime_ms==b.now);
        assert(!r&&b.discovery==0&&b.select==1&&b.close==1&&b.main_reads==1&&b.remote_reads==1&&b.writes==1);}
    bus_t b;nrf_topology_snapshot_t t;nrf_remote_sensor_cache_t c[8];topo(&b,&t,2);t.main.state=NRF_TOPOLOGY_NODE_ABSENT;cache(&t,c,2);nrf_sensor_transport_controller_t k=ctl();nrf_sensor_transport_io_t i=io(&b);bool r=false;
    assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,2,&c[2],&r)==ESP_OK&&b.main_reads==0&&b.discovery==0);
}
static void test_retry_timeout_and_faults(void){
    bus_t b;nrf_topology_snapshot_t t;nrf_remote_sensor_cache_t c[8];bool r;nrf_sensor_transport_controller_t k;nrf_sensor_transport_io_t i;
    topo(&b,&t,1);cache(&t,c,1);b.mode=M_FOREIGN;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,1,&c[1],&r)==ESP_OK&&b.writes==2&&b.snaps>=3);
    topo(&b,&t,4);cache(&t,c,4);b.mode=M_PENDING;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,4,&c[4],&r)==ESP_ERR_TIMEOUT&&c[4].state==NRF_SENSOR_CACHE_TIMEOUT&&b.now==500&&b.close==1);
    topo(&b,&t,4);cache(&t,c,4);b.mode=M_PROTOCOL;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,4,&c[4],&r)==ESP_ERR_INVALID_RESPONSE&&c[4].state==NRF_SENSOR_CACHE_PROTOCOL_ERROR&&r&&b.close==1);
    topo(&b,&t,4);cache(&t,c,4);b.mode=M_NACK;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,4,&c[4],&r)==ESP_ERR_NOT_FOUND&&c[4].state==NRF_SENSOR_CACHE_IDENTITY_STALE&&r&&b.close==1);
    topo(&b,&t,4);cache(&t,c,4);b.main_bad_epoch=true;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,4,&c[4],&r)==ESP_ERR_INVALID_RESPONSE&&b.remote_reads==0&&r&&b.close==1);
    topo(&b,&t,4);cache(&t,c,4);b.fail_close=true;b.mux_fault=true;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,4,&c[4],&r)==ESP_FAIL&&c[4].state==NRF_SENSOR_CACHE_MUX_FAULT&&r);
    topo(&b,&t,7);cache(&t,c,7);b.fail_select=true;k=ctl();i=io(&b);r=false;assert(nrf_sensor_transport_refresh_channel(&k,&i,&t,7,&c[7],&r)==ESP_FAIL&&b.close==1&&b.main_reads==0&&b.discovery==0);
}
int main(void){test_cache_epoch();test_success_and_no_discovery();test_retry_timeout_and_faults();return 0;}
