#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "nrf_cs_measurement_quality.h"
#include "nrf_cs_distance_method.h"
#include "nrf_cs_pair_runtime_zephyr_bridge.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_cs_rf_diagnostics.h"

#define ARG_UNUSED(x) (void)(x)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define RAS_FEAT_REALTIME_RD 1U
#define NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS 2
#define NRF_CS_CHANNEL_INDEX_OFFSET 2U
#define CS_DE_NUM_CHANNELS 75U
#define NRF_CS_TONE_OK_THRESHOLD 15U
#define CS_DE_TONE_QUALITY_OK 0
#define CS_DE_TONE_QUALITY_BAD 1
#define CS_DE_QUALITY_OK 0
#define BT_LE_CS_CHANNEL_BIT_GET(map,c) ((map[(c)/8] >> ((c)%8)) & 1U)
typedef int cs_de_quality_t;
#define NRF_CS_RANGING_COUNTER_NONE 0xffffU
#define BT_HCI_OP_LE_CS_MAIN_MODE_1 1
#define BT_HCI_OP_LE_CS_MAIN_MODE_2 2
#define BT_HCI_LE_CS_TONE_QUALITY_HIGH 0
#define BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL 0
#define BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE 127
#define BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE INT16_MIN
#define BT_HCI_ERR_SUCCESS 0
#define BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1 0
#define BT_CONN_LE_CS_PROCEDURE_COMPLETE 0
#define BT_CONN_LE_CS_PROCEDURE_ABORTED 15
#define BT_CONN_LE_CS_SUBEVENT_ABORTED 15
struct net_buf_simple { unsigned len; uint8_t data[128]; };
struct bt_le_cs_subevent_step { uint8_t mode,channel,data_len; const uint8_t *data; };
#pragma pack(push,1)
struct bt_hci_le_cs_step_data_mode_1 {
    uint8_t packet_quality_aa_check:4,packet_quality_bit_errors:4;
    uint8_t packet_nadm,packet_rssi;
    union { int16_t toa_tod_initiator,tod_toa_reflector; };
    uint8_t packet_antenna;
};
struct bt_hci_le_cs_step_data_mode_0_reflector {
    uint8_t packet_quality_aa_check:4,packet_quality_bit_errors:4;
    uint8_t packet_rssi,packet_antenna;
};
struct bt_hci_le_cs_step_data_tone_info { uint8_t phase_correction_term[3];uint8_t quality_indicator:4,extension_indicator:4; };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4200) /* Match the SDK's C99 flexible-array wire layout. */
#endif
struct bt_hci_le_cs_step_data_mode_2 { uint8_t antenna_permutation_index; struct bt_hci_le_cs_step_data_tone_info tone_info[]; };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#pragma pack(pop)
struct ras_ranging_header { int8_t selected_tx_power; uint8_t antenna_paths_mask; };
struct bt_le_cs_iq_sample { int16_t i,q; };
typedef struct {
    unsigned n_ap,role,rtt_count; int32_t rtt_accumulated_half_ns;
    struct {float i_local[75],q_local[75],i_remote[75],q_remote[75];} iq_tones[1];
    int tone_quality[1];
    struct {float ifft,phase_slope,rtt,best;} distance_estimates[1];
} cs_de_report_t;
#define BT_CONN_LE_CS_ROLE_INITIATOR 0
struct bt_conn_le_cs_procedure_enable_complete { int8_t selected_tx_power;unsigned tone_antenna_config_selection; };
struct bt_conn_le_cs_subevent_result {
    struct {int8_t reference_power_level;uint8_t num_antenna_paths;
        unsigned procedure_done_status,subevent_done_status,procedure_abort_reason,subevent_abort_reason;
        uint16_t procedure_counter;} header;
    struct net_buf_simple *step_data_buf;
};
struct bt_conn { int unused; };
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
#include "production_types.inc"

static nrf_cs_pair_adapter_t s_adapter;
static nrf_cs_step_t s_timeout_step;
static struct k_work_delayable s_timeout_work;
static bool s_realtime_ras, s_local_complete, s_peer_complete;
static uint16_t s_local_counter, s_peer_counter, s_ready_counter;
static struct net_buf_simple s_local_steps, s_peer_owned;
static nrf_cs_rf_diagnostics_t s_rf_diagnostics;
static int s_rf_tx_power,s_rf_status,s_rf_flags;
static uint16_t s_iq_counts[CS_DE_NUM_CHANNELS];
static cs_de_report_t s_de_report;
static struct {uint8_t channel_map[10];} s_cs_config;
static bool s_ras_skip,s_ras_malformed,s_ras_bad_paths;
static unsigned s_de_calls;
static unsigned s_de_scenario;
static uint16_t s_confidence;
static uint16_t s_provider_flags;
static uint32_t s_rf_words[3];
static unsigned s_rf_signal_events,s_rf_quality_events;
static int s_reflector_procedure_active;
static nrf_cs_pair_adapter_complete_cb_t s_user_complete_cb;
static void *s_user_complete_context;
static uint8_t s_node_id=0;
static int s_execute_pending;
static bool s_started=true;
static struct bt_conn *s_incoming_conn;
static struct bt_conn *s_outgoing_conn;
static struct bt_conn s_connection;
static struct k_work s_execute_work;
static int64_t s_attempt_deadline_ms;
static uint64_t s_now, s_due;
static unsigned s_connect_delay, s_callbacks, s_secure_calls, s_cleanup_calls;
static unsigned s_estimate_delay;
static bool s_defer_timer, s_schedule_fail;
static uint32_t s_last_error;
static unsigned s_error_events;
static bool s_timer_armed;
static nrf_cs_outcome_t s_outcome;
static runtime_event_t s_events[24];
static unsigned s_count, s_taken;
static int s_event_queue;
static void atomic_set(int *v,int n) { *v=n; }
typedef int atomic_val_t;
static void atomic_or(int *v,int n) { *v |= n; }
static int atomic_get(const int *v) { return *v; }
static int64_t k_uptime_get(void) { return (int64_t)s_now; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)s_now; }
static int submit_cs_work(struct k_work *w) { (void)w;return 1; }
static void bt_conn_unref(struct bt_conn *c) { (void)c; }
static int k_msgq_get(int *q,runtime_event_t *e,int wait)
{ (void)q;(void)wait; if(s_taken==s_count)return -1;*e=s_events[s_taken++];return 0; }
static void nrf_log_runtime_zephyr_append_event(uint16_t id,uint32_t a,uint32_t b)
{ (void)a;if(id==NRF_LOG_EVENT_CS_EXECUTE_FAILED){s_last_error=b;++s_error_events;}
  if(id>=NRF_LOG_EVENT_CS_RF_SIGNAL && id<=NRF_LOG_EVENT_CS_RF_STATUS)s_rf_words[id-NRF_LOG_EVENT_CS_RF_SIGNAL]=b;
  if(id==NRF_LOG_EVENT_CS_RF_SIGNAL)++s_rf_signal_events;
  if(id==NRF_LOG_EVENT_CS_RF_QUALITY)++s_rf_quality_events;
}
static void nrf_watchdog_zephyr_checkin(int p) { (void)p; }
static int runtime_start_advertising(void) { return 0; }
static void runtime_reset_ranging(void)
{ s_local_complete=s_peer_complete=false; s_local_steps.len=s_peer_owned.len=0;
  nrf_cs_rf_reset(&s_rf_diagnostics);s_rf_tx_power=127;
  s_rf_status=(int)nrf_cs_rf_status_word(127,127,0,0,0,false);s_rf_flags=0; }
static int k_work_cancel_delayable(struct k_work_delayable *w)
{ (void)w; s_timer_armed=false; return 0; }
static int reschedule_cs_work(struct k_work_delayable *w,uint32_t ms)
{ (void)w; if(s_schedule_fail)return -EBUSY;s_due=s_now+ms; s_timer_armed=true; return 1; }
static bool runtime_active_step(nrf_cs_step_t *step)
{ const nrf_cs_step_t *s=nrf_cs_pair_adapter_active_step(&s_adapter);if(!s)return false;*step=*s;return true; }
static bool runtime_step_matches_active(const nrf_cs_step_t *s)
{ return nrf_cs_pair_runtime_bridge_step_matches(&s_adapter,s); }
static bool runtime_active_step_for_conn(struct bt_conn *c,nrf_cs_step_t *s)
{ return c==s_outgoing_conn && runtime_active_step(s); }
static struct bt_le_cs_iq_sample bt_le_cs_parse_pct(const uint8_t *p)
{ (void)p;return (struct bt_le_cs_iq_sample){1,1}; }
static void net_buf_simple_reset(struct net_buf_simple *b) { b->len=0; }
static size_t net_buf_simple_tailroom(struct net_buf_simple *b) { return sizeof(b->data)-b->len; }
static void net_buf_simple_add_mem(struct net_buf_simple *b,const void *p,size_t n)
{ memcpy(b->data+b->len,p,n);b->len+=(unsigned)n; }
static uint16_t bt_ras_rreq_get_ranging_counter(uint16_t c) { return c & 0xfffU; }
static void runtime_post_event(runtime_event_type_t t,const nrf_cs_step_t *s,uint32_t v,int e,struct bt_conn *c)
{ (void)t;(void)s;(void)v;(void)e;(void)c; }
static void maybe_request_on_demand_data(void) {}
static void configure_reflector_connection(struct bt_conn *c) { (void)c; }
static void configure_reflector_procedure(struct bt_conn *c,uint8_t id) { (void)c;(void)id; }
/* The console trace hooks of nrf_cs_trace_zephyr.h, recorded so the probe can check what the handlers report. */
typedef struct { const char *label; uint32_t attempt; int status; uint8_t phase; } probe_trace_event_t;
static probe_trace_event_t s_trace_events[64];
static unsigned s_trace_event_count,s_trace_execute_count,s_trace_timeout_count,s_trace_result_count,s_trace_rf_count;
static uint32_t s_trace_execute_budget,s_trace_execute_attempt;
static uint8_t s_trace_execute_initiator,s_trace_execute_reflector,s_trace_execute_try,s_trace_execute_pair;
static uint8_t s_trace_timeout_phase,s_trace_result_outcome,s_trace_result_phase;
static uint32_t s_trace_timeout_budget,s_trace_rf_signal,s_trace_rf_quality;
static int32_t s_trace_result_distance;
static uint16_t s_trace_result_confidence;
static void nrf_cs_trace_zephyr_execute(uint32_t id,uint8_t i,uint8_t r,uint8_t t,uint8_t p,uint32_t b)
{ ++s_trace_execute_count;s_trace_execute_attempt=id;s_trace_execute_initiator=i;s_trace_execute_reflector=r;
  s_trace_execute_try=t;s_trace_execute_pair=p;s_trace_execute_budget=b; }
static void nrf_cs_trace_zephyr_event(const char *label,uint32_t id,int status,uint8_t phase)
{ if(s_trace_event_count<64)s_trace_events[s_trace_event_count]=(probe_trace_event_t){label,id,status,phase};
  ++s_trace_event_count; }
static void nrf_cs_trace_zephyr_timeout(uint32_t id,uint8_t phase,uint32_t budget)
{ (void)id;++s_trace_timeout_count;s_trace_timeout_phase=phase;s_trace_timeout_budget=budget; }
static bool s_trace_result_has_timing;
static nrf_cs_pair_adapter_timing_t s_trace_result_timing;
static void nrf_cs_trace_zephyr_result(uint32_t id,uint8_t outcome,int32_t dist,uint16_t conf,uint8_t phase,
    const nrf_cs_pair_adapter_timing_t *timing)
{ (void)id;++s_trace_result_count;s_trace_result_outcome=outcome;s_trace_result_distance=dist;
  s_trace_result_confidence=conf;s_trace_result_phase=phase;
  s_trace_result_has_timing=timing!=NULL;if(timing!=NULL)s_trace_result_timing=*timing; }
static unsigned s_trace_estimates_count;
static float s_trace_est[4];
static void nrf_cs_trace_zephyr_estimates(uint32_t id,float ifft,float phase,float rtt,float best)
{ (void)id;++s_trace_estimates_count;s_trace_est[0]=ifft;s_trace_est[1]=phase;s_trace_est[2]=rtt;s_trace_est[3]=best; }
static void nrf_cs_trace_zephyr_rf(uint32_t id,uint32_t signal,uint32_t quality)
{ (void)id;++s_trace_rf_count;s_trace_rf_signal=signal;s_trace_rf_quality=quality; }
#include "runtime_emit_rf_diagnostics.inc"
#include "runtime_adapter_complete.inc"
#include "report_ranging_header.inc"
#include "accumulate_mean.inc"
#include "collect_pbr_step.inc"
#include "collect_rtt_step.inc"
#include "report_step_data.inc"
#include "good_tone_channel_count.inc"
static void bt_ras_rreq_rd_subevent_data_parse(struct net_buf_simple *a,struct net_buf_simple *b,
    unsigned role,bool (*header_cb)(struct ras_ranging_header *,void *),void *sub_cb,
    bool (*step_cb)(struct bt_le_cs_subevent_step *,struct bt_le_cs_subevent_step *,void *),void *context)
{
    (void)a;(void)b;(void)role;(void)sub_cb;
    if(s_ras_skip)return;
    struct ras_ranging_header h={.selected_tx_power=-8,.antenna_paths_mask=1};
    if(s_ras_bad_paths)h.antenna_paths_mask=2;
    (void)header_cb(&h,context);
    uint8_t tone[5]={0};
    for(uint8_t i=2;i<18;++i){
        struct bt_le_cs_subevent_step step={2,i,sizeof(tone),tone};
        if(!step_cb(&step,&step,context))return;
    }
    if(s_ras_malformed){
        struct bt_le_cs_subevent_step truncated={2,18,0,tone};
        (void)step_cb(&truncated,&truncated,context);
    }
}
static cs_de_quality_t cs_de_calc(cs_de_report_t *report)
{
    ++s_de_calls;s_now+=s_estimate_delay;
    report->distance_estimates[0].ifft=1;report->distance_estimates[0].phase_slope=2;
    report->distance_estimates[0].rtt=1;report->distance_estimates[0].best=1;
    if(s_de_scenario==1)report->distance_estimates[0].best=NAN;
    if(s_de_scenario==2)report->distance_estimates[0].best=INFINITY;
    if(s_de_scenario==3)report->distance_estimates[0].best=-1;
    if(s_de_scenario==4){report->distance_estimates[0].ifft=NAN;report->distance_estimates[0].phase_slope=NAN;}
    if(s_de_scenario==5){report->distance_estimates[0].ifft=NAN;report->distance_estimates[0].best=2;}
    return CS_DE_QUALITY_OK;
}
#include "estimate_measurement.inc"
#include "timeout_work_handler.inc"
#include "cleanup_work_handler.inc"
#include "maybe_complete_measurement.inc"
#include "event_work_handler.inc"
#include "execute_work_handler.inc"
#include "nrf_cs_pair_runtime_zephyr_execute.inc"
#include "procedure_enable_complete_cb.inc"
#include "subevent_data_cb.inc"

static int connect_op(void *c,nrf_cs_node_id_t id)
{ (void)c;(void)id;s_now+=s_connect_delay;return 0; }
static int secure_op(void *c) { (void)c;++s_secure_calls;return 0; }
static int noop(void *c) { (void)c;return 0; }
static int config_op(void *c,bool realtime) { (void)realtime;return noop(c); }
static int cleanup_op(void *c) { (void)c;++s_cleanup_calls;return 0; }
#include "op_now_ms.inc"
static void completed(const nrf_cs_step_t *step,const nrf_cs_measurement_t *m,void *c)
{ (void)step;(void)c;++s_callbacks;s_outcome=m->outcome;s_confidence=m->confidence;s_provider_flags=m->provider_quality_flags; }
static void deliver(runtime_event_type_t type,const nrf_cs_step_t *step)
{
    s_count=1;s_taken=0;
    s_events[0]=(runtime_event_t){.type=type,.has_step=true,.step=*step,.value=1};
    event_work_handler(NULL);
}
static void start_queued(unsigned sync_delay,unsigned queue_delay)
{
    const nrf_cs_pair_adapter_ops_t ops={.connect=connect_op,.secure=secure_op,
        .exchange_mtu=noop,.discover_ras=noop,.read_features=noop,
        .configure_ranging_data=config_op,.exchange_capabilities=noop,
        .create_config=noop,.enable_cs_security=noop,.start_procedure=noop,.cleanup=cleanup_op,.now_ms=op_now_ms};
    assert(nrf_cs_pair_adapter_init(&s_adapter,&ops)==0);
    s_now=0;s_connect_delay=sync_delay;s_callbacks=s_secure_calls=s_cleanup_calls=0;
    s_due=0;s_timer_armed=false;s_count=s_taken=0;runtime_reset_ranging();
    s_error_events=0;s_last_error=0;
    s_rf_signal_events=s_rf_quality_events=0;memset(s_rf_words,0,sizeof(s_rf_words));
    s_trace_event_count=s_trace_execute_count=s_trace_timeout_count=s_trace_result_count=s_trace_rf_count=0;
    s_trace_estimates_count=0;
    s_outgoing_conn=&s_connection;s_incoming_conn=NULL;
    s_ras_skip=s_ras_malformed=s_ras_bad_paths=false;s_de_calls=s_de_scenario=0;
    s_confidence=0;memset(&s_cs_config,255,sizeof(s_cs_config));
    s_timeout_step=(nrf_cs_step_t){.cycle_id=1,.membership_generation=1,
        .attempt_id=1,.pair_index=0,.attempt_number=1,.initiator=0,.reflector=1,.timeout_ms=500};
    s_execute_pending=0;
    assert(nrf_cs_pair_runtime_zephyr_execute(&s_timeout_step,completed,NULL)==0);
    s_now=queue_delay;
    execute_work_handler(NULL);
}
static void start(unsigned sync_delay) { start_queued(sync_delay,0); }
static void advance_to_procedure(void)
{
    const runtime_event_type_t phases[]={RUNTIME_EVENT_CONNECTED,RUNTIME_EVENT_SECURITY_READY,
        RUNTIME_EVENT_MTU_READY,RUNTIME_EVENT_DISCOVERY_READY,RUNTIME_EVENT_FEATURES_READY,
        RUNTIME_EVENT_CAPABILITIES_READY,RUNTIME_EVENT_CONFIG_READY,RUNTIME_EVENT_CS_SECURITY_READY};
    for(unsigned i=0;i<8;++i)deliver(phases[i],&s_timeout_step);
    assert(nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_PROCEDURE);
}
static nrf_cs_outcome_t finish_at(unsigned delay,unsigned elapsed)
{
    start(delay);advance_to_procedure();s_now=elapsed;
    printf("elapsed=%u sync_start_delay=%u timer_due=%llu ",elapsed,delay,(unsigned long long)s_due);
    /* Correct fake timer: run due timeout before delivering newly arrived data. */
    if(s_timer_armed && s_now>=s_due && !s_defer_timer)timeout_work_handler(NULL);
    s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);
    deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_callbacks==1 && !nrf_cs_pair_adapter_is_active(&s_adapter));
    printf("outcome=%u\n",(unsigned)s_outcome);return s_outcome;
}
int main(void)
{
    unsigned gaps=0;
    const unsigned elapsed[]={499,500,501};
    for(unsigned i=0;i<3;++i) {
        nrf_cs_outcome_t expected=elapsed[i]<500?NRF_CS_OUTCOME_SUCCESS:NRF_CS_OUTCOME_TIMEOUT;
        if(finish_at(0,elapsed[i])!=expected)++gaps;
    }
    if(finish_at(80,501)!=NRF_CS_OUTCOME_TIMEOUT)++gaps;
    s_defer_timer=true;
    if(finish_at(0,501)!=NRF_CS_OUTCOME_TIMEOUT)++gaps;
    s_defer_timer=false;
    s_estimate_delay=2;
    if(finish_at(0,499)!=NRF_CS_OUTCOME_TIMEOUT)++gaps;
    s_estimate_delay=0;
    s_schedule_fail=true;
    start(0);
    printf("timer scheduling failure callbacks=%u active=%u\n",s_callbacks,
        nrf_cs_pair_adapter_is_active(&s_adapter)?1U:0U);
    if(s_callbacks!=1 || s_outcome!=NRF_CS_OUTCOME_TRANSPORT_ERROR ||
        nrf_cs_pair_adapter_is_active(&s_adapter))++gaps;
    s_schedule_fail=false;
    start(0);s_now=501;s_count=1;s_taken=0;
    s_events[0]=(runtime_event_t){.type=RUNTIME_EVENT_TRANSPORT_ERROR,.has_step=true,
        .step=s_timeout_step,.status=-EIO};
    event_work_handler(NULL);cleanup_work_handler(NULL);
    printf("late raw error preserved=%u outcome=%u\n",
        s_error_events==1 && s_last_error==(uint32_t)-EIO?1U:0U,(unsigned)s_outcome);
    if(s_error_events!=1 || s_last_error!=(uint32_t)-EIO || s_outcome!=NRF_CS_OUTCOME_TIMEOUT)++gaps;
    start_queued(0,501);
    printf("expired queued start callbacks=%u cleanup=%u timer=%u\n",s_callbacks,s_cleanup_calls,s_timer_armed?1U:0U);
    if(s_callbacks!=1 || s_outcome!=NRF_CS_OUTCOME_TIMEOUT || s_timer_armed ||
        nrf_cs_pair_adapter_is_active(&s_adapter))++gaps;
    const char *fields[]={"membership_generation","pair_index","attempt_number","initiator","reflector"};
    for(unsigned i=0;i<5;++i) {
        start(0);nrf_cs_step_t wrong=s_timeout_step;
        switch(i) {
        case 0:++wrong.membership_generation;break;
        case 1:++wrong.pair_index;break;
        case 2:++wrong.attempt_number;break;
        case 3:++wrong.initiator;break;
        case 4:++wrong.reflector;break;
        }
        deliver(RUNTIME_EVENT_CONNECTED,&wrong);
        bool rejected=s_secure_calls==0 && s_callbacks==0 &&
            nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_CONNECTING;
        printf("bridge token=%s rejected=%u\n",fields[i],rejected?1U:0U);
        if(!rejected)++gaps;
    }
    start(0);advance_to_procedure();
    const struct bt_conn_le_cs_procedure_enable_complete power={.selected_tx_power=4,
        .tone_antenna_config_selection=BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1};
    procedure_enable_complete_cb(s_outgoing_conn,0,(void *)&power);
    cs_de_report_t report={0};
    struct ras_ranging_header header={.selected_tx_power=-8,.antenna_paths_mask=1};
    assert(report_ranging_header(&header,&report));
    const uint8_t local_mode0[5]={0,(uint8_t)-60,1,0,0},peer_mode0[3]={0,(uint8_t)-80,1};
    struct bt_le_cs_subevent_step ml={0,0,sizeof(local_mode0),local_mode0};
    struct bt_le_cs_subevent_step mp={0,0,sizeof(peer_mode0),peer_mode0};
    assert(report_step_data(&ml,&mp,&report)); /* RAS delivers mode0 before RTT/PBR steps. */
    struct bt_hci_le_cs_step_data_mode_1 local={.packet_rssi=(uint8_t)-60,.packet_antenna=1};
    struct bt_hci_le_cs_step_data_mode_1 peer={.packet_rssi=(uint8_t)-80,.packet_antenna=1,.packet_quality_aa_check=1};
    struct bt_le_cs_subevent_step ls={1,2,sizeof(local),(void *)&local};
    struct bt_le_cs_subevent_step ps={1,2,sizeof(peer),(void *)&peer};
    assert(report_step_data(&ls,&ps,&report));
    uint8_t local_tone[5]={0},peer_tone[5]={0};
    ls=(struct bt_le_cs_subevent_step){2,2,sizeof(local_tone),local_tone};
    ps=(struct bt_le_cs_subevent_step){2,2,sizeof(peer_tone),peer_tone};
    assert(report_step_data(&ls,&ps,&report));
    local_tone[4]=2;ls.channel=ps.channel=3;
    assert(report_step_data(&ls,&ps,&report));
    s_rf_diagnostics.good_channels=1;
    struct bt_conn_le_cs_subevent_result radio={.header={.reference_power_level=-65,
        .num_antenna_paths=1,.procedure_done_status=BT_CONN_LE_CS_PROCEDURE_COMPLETE,.procedure_counter=7}};
    subevent_data_cb(s_outgoing_conn,&radio);
    s_ras_skip=true;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_rf_signal_events==1 && s_rf_quality_events==1);
    assert((int8_t)s_rf_words[0]==-60 && (int8_t)(s_rf_words[0]>>8)==-80);
    assert((int8_t)(s_rf_words[0]>>16)==4 && (int8_t)(s_rf_words[0]>>24)==-8);
    assert((s_rf_words[1]&127U)==50 && ((s_rf_words[1]>>7)&127U)==0);
    assert((s_rf_words[1]>>25)&NRF_CS_RF_PEER_PACKET_ERROR);
    printf("RF raw fields: local_rssi=-60 peer_rssi=-80 local_tx=4 peer_tx=-8 unusable_tones=50/0%%\n");
    start(0);advance_to_procedure();ls.data_len=0;
    assert(!report_step_data(&ls,&ps,&report));
    assert(s_rf_diagnostics.flags&NRF_CS_RF_MALFORMED);
    procedure_enable_complete_cb(s_outgoing_conn,0,(void *)&power);
    radio.header.num_antenna_paths=2;radio.header.procedure_done_status=BT_CONN_LE_CS_PROCEDURE_ABORTED;
    radio.header.subevent_done_status=BT_CONN_LE_CS_SUBEVENT_ABORTED;
    radio.header.procedure_abort_reason=2;radio.header.subevent_abort_reason=2;
    subevent_data_cb(s_outgoing_conn,&radio);
    deliver(RUNTIME_EVENT_PROCEDURE_ABORTED,&s_timeout_step);cleanup_work_handler(NULL);
    assert((s_rf_words[1]>>25)&NRF_CS_RF_ANTENNA_MISMATCH);
    assert((s_rf_words[1]>>25)&NRF_CS_RF_ABORTED);
    assert((s_rf_words[2]&15U)==2 && ((s_rf_words[2]>>8)&15U)==2);
    assert((int8_t)(s_rf_words[2]>>12)==-65);
    printf("RF aborted/path mismatch preserved; malformed steps rejected before casting\n");
    start(0);advance_to_procedure();s_ras_malformed=true;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    printf("Malformed RAS after good tones outcome=%u de_calls=%u\n",(unsigned)s_outcome,s_de_calls);
    assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && s_de_calls==0);
    for(unsigned invalid=1;invalid<=4;++invalid){
        start(0);advance_to_procedure();s_de_scenario=invalid;
        s_now=100;s_local_steps.len=s_peer_owned.len=1;
        deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
        cleanup_work_handler(NULL);
        if(invalid<4)assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && s_confidence==0);
        else assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_confidence==400);
        printf("DE boundary case=%u outcome=%u confidence=%u\n",invalid,(unsigned)s_outcome,s_confidence);
    }
    start(0);advance_to_procedure();s_ras_bad_paths=true;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && s_de_calls==0);
    /* Console trace: what the handlers report for one attempt, so a UART log shows the time of every stage. */
    start(0);
    assert(s_trace_execute_count==1 && s_trace_execute_budget==500 && s_trace_execute_attempt==1);
    assert(s_trace_execute_initiator==0 && s_trace_execute_reflector==1 && s_trace_execute_try==1 && s_trace_execute_pair==0);
    assert(s_trace_event_count==1 && !strcmp(s_trace_events[0].label,"EXECUTE_WORK_STARTED"));  /* the queue wait */
    advance_to_procedure();
    assert(s_trace_event_count==9);                       /* the work start and the eight stages */
    assert(!strcmp(s_trace_events[1].label,"CONNECTED") && s_trace_events[1].phase==NRF_CS_PAIR_PHASE_CONNECTING);
    assert(!strcmp(s_trace_events[2].label,"SECURITY_READY") && s_trace_events[2].phase==NRF_CS_PAIR_PHASE_SECURING);
    assert(!strcmp(s_trace_events[8].label,"CS_SECURITY_READY") && s_trace_events[8].phase==NRF_CS_PAIR_PHASE_CS_SECURITY);
    assert(s_trace_events[2].attempt==1 && s_trace_events[2].status==0);
    s_now=501;timeout_work_handler(NULL);
    assert(s_trace_timeout_count==1 && s_trace_timeout_phase==NRF_CS_PAIR_PHASE_PROCEDURE && s_trace_timeout_budget==500);
    cleanup_work_handler(NULL);
    assert(s_trace_result_count==1 && s_trace_result_outcome==NRF_CS_OUTCOME_TIMEOUT && s_trace_rf_count==1);
    start(0);advance_to_procedure();s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_trace_timeout_count==0 && s_trace_result_count==1 && s_trace_result_outcome==NRF_CS_OUTCOME_SUCCESS);
    /* All three estimates of the library reach the trace, not only "best": the method is picked from them. */
    assert(s_trace_estimates_count==1 && s_trace_est[0]==1.0f && s_trace_est[1]==2.0f && s_trace_est[2]==1.0f && s_trace_est[3]==1.0f);
    assert(s_trace_result_confidence==s_confidence && s_trace_rf_count==1);
    assert(s_trace_result_has_timing && s_trace_result_timing.valid);
    assert(s_trace_result_timing.measured_ms==100 && s_trace_result_timing.cleanup_ms==0);
    assert(s_trace_result_timing.total_ms==100 && !s_trace_result_timing.budget_exceeded);
    /* Execute the production estimator and completion path for all three method choices. */
    const unsigned scenarios[]={0,5,4,1};
    const uint16_t expected_methods[]={0x10U,0x20U,0x40U,0U};
    for(unsigned i=0;i<4;++i){
        start(0);advance_to_procedure();s_de_scenario=scenarios[i];s_now=100;
        s_local_steps.len=s_peer_owned.len=1;
        deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
        s_now=120;cleanup_work_handler(NULL);
        assert((s_provider_flags & 0x70U)==expected_methods[i]);
        assert(s_outcome==(i==3?NRF_CS_OUTCOME_OUTLIER:NRF_CS_OUTCOME_SUCCESS));
        assert(s_trace_result_has_timing && s_trace_result_timing.valid);
        assert(s_trace_result_timing.measured_ms==100 && s_trace_result_timing.cleanup_ms==20);
    }
    /* A result before the deadline survives the actual timeout handler during delayed cleanup. */
    start(0);advance_to_procedure();s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    s_now=500;timeout_work_handler(NULL);s_now=600;cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_timing.valid);
    assert(s_trace_result_timing.measured_ms==100 && s_trace_result_timing.cleanup_ms==500);
    assert(s_trace_result_timing.total_ms==600 && s_trace_result_timing.budget_exceeded);
    assert(s_trace_result_timing.timeout_during_cleanup);
    /* Queue delay belongs to the same accepted-at deadline as the elapsed trace, including its exact boundary. */
    const unsigned cleanup_at[]={500,501,550};
    for(unsigned i=0;i<3;++i){
        start_queued(0,80);advance_to_procedure();s_now=100;s_local_steps.len=s_peer_owned.len=1;
        deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
        s_now=500;timeout_work_handler(NULL);s_now=cleanup_at[i];cleanup_work_handler(NULL);
        printf("queued timing measured=%u cleanup=%u total=%u over_budget=%u\n",
            (unsigned)s_trace_result_timing.measured_ms,(unsigned)s_trace_result_timing.cleanup_ms,
            (unsigned)s_trace_result_timing.total_ms,s_trace_result_timing.budget_exceeded?1U:0U);
        assert(s_trace_result_timing.measured_ms==100 && s_trace_result_timing.cleanup_ms==cleanup_at[i]-100);
        assert(s_trace_result_timing.total_ms==cleanup_at[i]);
        assert(s_trace_result_timing.budget_exceeded==(cleanup_at[i]>500));
        assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_timing.timeout_during_cleanup);
    }
    start(0);s_now=60;s_count=1;s_taken=0;                /* an error code of a late event shows in the trace too */
    s_events[0]=(runtime_event_t){.type=RUNTIME_EVENT_TRANSPORT_ERROR,.has_step=true,.step=s_timeout_step,.status=9};
    event_work_handler(NULL);
    assert(s_trace_event_count==2 && !strcmp(s_trace_events[1].label,"TRANSPORT_ERROR") && s_trace_events[1].status==9);
    printf("console trace hooks verified\n");
    printf("PRODUCTION_HANDLER_ACCEPTANCE_GAPS=%u\n",gaps);
    return gaps?1:0;
}
