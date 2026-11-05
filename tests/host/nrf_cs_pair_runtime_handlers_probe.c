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
#include "nrf_cs_tone_stats.h"
#include "nrf_cs_iq_gap_fill.h"
#include "nrf_cs_ifft_quality.h"
#include "nrf_cs_cycle_runtime.h"
#include "nrf_cs_pair_runtime_zephyr_bridge.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_cs_rf_diagnostics.h"
#include "nrf_cs_event_overflow.h"

#define ARG_UNUSED(x) (void)(x)
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define BT_LE_SCAN_PASSIVE NULL
#define NRF_CS_EVENT_QUEUE_DEPTH 24U
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define RAS_FEAT_REALTIME_RD 1U
#define NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS 2
#define NRF_CS_CHANNEL_INDEX_OFFSET 2U
#define CS_DE_NUM_CHANNELS 75U
#define CONFIG_BT_CS_DE_NFFT_SIZE 512U
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
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1 0
#define BT_CONN_LE_CS_PROCEDURE_COMPLETE 0
#define BT_CONN_LE_CS_PROCEDURE_ABORTED 15
#define BT_CONN_LE_CS_SUBEVENT_ABORTED 15
#define BT_CONN_LE_CS_PROCEDURES_DISABLED 0
#define BT_CONN_LE_CS_PROCEDURES_ENABLED 1
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
struct bt_conn_le_cs_procedure_enable_complete { int8_t selected_tx_power;unsigned tone_antenna_config_selection,state; };
struct bt_conn_le_cs_subevent_result {
    struct {int8_t reference_power_level;uint8_t num_antenna_paths;
        unsigned procedure_done_status,subevent_done_status,procedure_abort_reason,subevent_abort_reason;
        uint16_t procedure_counter;} header;
    struct net_buf_simple *step_data_buf;
};
struct bt_conn { unsigned refs; };
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
#include "production_types.inc"
static nrf_cs_pair_runtime_final_hint_cb_t s_relay_final_hint_cb;
static nrf_cs_pair_runtime_released_cb_t s_relay_released_cb;

static nrf_cs_pair_adapter_t s_adapter;
static nrf_cs_step_t s_timeout_step;
static struct k_work_delayable s_timeout_work;
static bool s_realtime_ras, s_local_complete, s_peer_complete;
static uint16_t s_local_counter, s_peer_counter, s_ready_counter;
static struct net_buf_simple s_local_steps, s_peer_owned, s_peer_rx;
static bool s_on_demand_request_started;
static int s_local_data_frozen,s_peer_data_frozen;
static int s_expected_counter=NRF_CS_RANGING_COUNTER_NONE;
static nrf_cs_rf_diagnostics_t s_rf_diagnostics;
static int s_rf_tx_power,s_rf_status,s_rf_flags;
static uint16_t s_iq_counts[CS_DE_NUM_CHANNELS];
static float s_iq_ifft_scratch[2U * 512U];
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
static int s_radio_enabled=1;
static unsigned s_disconnect_calls,s_stop_advertising_calls;
static int s_disconnect_result;
static uint8_t s_last_disconnect_reason;
static unsigned s_reflector_config_calls;
static unsigned s_release_notifications;
static nrf_cs_cycle_runtime_t s_release_cycle;
static unsigned s_advertising_starts;
static bool s_final_hint;
static bool final_hint(const nrf_cs_measurement_t *measurement) { (void)measurement;return s_final_hint; }
static void released_hint(void) { ++s_release_notifications;nrf_cs_cycle_runtime_peer_released(&s_release_cycle); }
static struct bt_conn *s_disconnected_conn;
static nrf_cs_pair_adapter_complete_cb_t s_user_complete_cb;
static void *s_user_complete_context;
static uint8_t s_node_id=0;
static int s_execute_pending;
static bool s_started=true;
static struct bt_conn *s_incoming_conn;
static bool s_advertising;
static bool s_scanning,s_controller_advertising;
static uint8_t s_scan_target;
static int s_advertising_stop_result;
static unsigned s_scan_starts;
static struct bt_conn *s_outgoing_conn;
static bool s_outgoing_connected;
static int s_disconnect_retry_pending;
static int s_incoming_disconnect_pending;
static struct bt_conn s_connection;
static struct k_work s_execute_work;
static struct k_work s_event_work;
static struct k_work s_cleanup_work;
static struct k_work *s_failed_submit;
static bool s_use_production_cleanup;
static struct k_spinlock {int unused;} s_cancel_lock;
static struct k_spinlock s_event_queue_overflow_lock;
static uint32_t s_event_queue_overflow_total;
typedef unsigned k_spinlock_key_t;
static unsigned s_cancel_lock_depth;
static unsigned s_event_submits;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{ (void)lock;assert(s_cancel_lock_depth==0);++s_cancel_lock_depth;return 0; }
static void k_spin_unlock(struct k_spinlock *lock,k_spinlock_key_t key)
{ (void)lock;(void)key;assert(s_cancel_lock_depth==1);--s_cancel_lock_depth; }
static nrf_cs_step_t s_cancel_step;
static bool s_cancel_pending;
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
static bool s_capture_posts;
static bool s_worker_during_incoming_unref;
static void event_work_handler(struct k_work *work);
static int s_event_queue;
static void atomic_set(int *v,int n) { *v=n; }
typedef int atomic_val_t;
static void atomic_or(int *v,int n) { *v |= n; }
static int atomic_get(const int *v) { return *v; }
static bool atomic_cas(int *v,int old_value,int new_value)
{ if(*v!=old_value)return false;*v=new_value;return true; }
static int64_t k_uptime_get(void) { return (int64_t)s_now; }
static uint32_t k_uptime_get_32(void) { return (uint32_t)s_now; }
static int submit_cs_work(struct k_work *w) { if(w==&s_event_work)++s_event_submits;return w==s_failed_submit?-EBUSY:1; }
static struct bt_conn *bt_conn_ref(struct bt_conn *c) { ++c->refs;return c; }
static void bt_conn_unref(struct bt_conn *c) {
    if(s_worker_during_incoming_unref && c==s_incoming_conn){
        s_worker_during_incoming_unref=false;
        const unsigned releases=s_release_notifications,starts=s_advertising_starts;
        event_work_handler(NULL);
        assert(s_release_notifications==releases && s_advertising_starts==starts);
    }
    assert(c->refs>0);--c->refs;
}
static int k_msgq_get(int *q,runtime_event_t *e,int wait)
{ (void)q;(void)wait; if(s_taken==s_count)return -1;*e=s_events[s_taken++];return 0; }
static int k_msgq_put(int *q,const runtime_event_t *e,int wait)
{ (void)q;(void)wait;
  /* Existing probe scenarios opt into queued callback delivery explicitly. */
  if(!s_capture_posts){if(e->conn!=NULL)bt_conn_unref(e->conn);return 0;}
  if(s_count==NRF_CS_EVENT_QUEUE_DEPTH)return -ENOBUFS;
  s_events[s_count++]=*e;return 0; }
static void nrf_log_runtime_zephyr_append_event(uint16_t id,uint32_t a,uint32_t b)
{ (void)a;if(id==NRF_LOG_EVENT_CS_EXECUTE_FAILED){s_last_error=b;++s_error_events;}
  if(id>=NRF_LOG_EVENT_CS_RF_SIGNAL && id<=NRF_LOG_EVENT_CS_RF_STATUS)s_rf_words[id-NRF_LOG_EVENT_CS_RF_SIGNAL]=b;
  if(id==NRF_LOG_EVENT_CS_RF_SIGNAL)++s_rf_signal_events;
  if(id==NRF_LOG_EVENT_CS_RF_QUALITY)++s_rf_quality_events;
}
static void nrf_watchdog_zephyr_checkin(int p) { (void)p; }
static int runtime_start_advertising(void) { ++s_advertising_starts;return 0; }
static int bt_le_adv_stop(void) {
    ++s_stop_advertising_calls;
    if(s_advertising_stop_result==0 || s_advertising_stop_result==-EALREADY)s_controller_advertising=false;
    return s_advertising_stop_result;
}
static void scan_device_found(void) {}
static int bt_le_scan_start(const void *params,void (*callback)(void))
{ (void)params;(void)callback;++s_scan_starts;return 0; }
#include "runtime_stop_advertising.inc"
static void runtime_stop_scan(void) {}
static int bt_conn_disconnect(struct bt_conn *conn,uint8_t reason)
{ assert(reason==BT_HCI_ERR_REMOTE_USER_TERM_CONN || reason==NRF_CS_CYCLE_RELEASE_REASON);
  s_last_disconnect_reason=reason;++s_disconnect_calls;s_disconnected_conn=conn;return s_disconnect_result; }
static void runtime_reset_ranging(void);
static int op_cleanup(void *context);
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
#include "runtime_post_event.inc"
#include "runtime_reset_procedure.inc"
#include "runtime_reset_ranging.inc"
#include "runtime_counter_matches.inc"
#include "op_connect.inc"
#include "runtime_take_cancel.inc"
#include "nrf_cs_pair_runtime_zephyr_cancel.inc"
#include "ranging_data_received_cb.inc"
#include "ranging_data_ready_cb.inc"
#include "connected_cb.inc"
#include "disconnected_cb.inc"
static void maybe_request_on_demand_data(void) {}
static void configure_reflector_connection(struct bt_conn *c) { (void)c;++s_reflector_config_calls; }
static void configure_reflector_procedure(struct bt_conn *c,uint8_t id) { (void)c;(void)id;++s_reflector_config_calls; }
/* The console trace hooks of nrf_cs_trace_zephyr.h, recorded so the probe can check what the handlers report. */
typedef struct { const char *label; uint32_t attempt; int status; uint8_t phase; } probe_trace_event_t;
static probe_trace_event_t s_trace_events[64];
static unsigned s_trace_event_count,s_trace_execute_count,s_trace_timeout_count,s_trace_result_count,s_trace_rf_count;
static uint32_t s_trace_execute_budget,s_trace_execute_attempt;
static uint8_t s_trace_execute_initiator,s_trace_execute_reflector,s_trace_execute_try,s_trace_execute_pair;
static uint8_t s_trace_timeout_phase,s_trace_result_outcome,s_trace_result_phase;
static uint32_t s_trace_timeout_budget,s_trace_rf_signal,s_trace_rf_quality,s_trace_result_attempt;
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
{ s_trace_result_attempt=id;++s_trace_result_count;s_trace_result_outcome=outcome;s_trace_result_distance=dist;
  s_trace_result_confidence=conf;s_trace_result_phase=phase;
  s_trace_result_has_timing=timing!=NULL;if(timing!=NULL)s_trace_result_timing=*timing; }
static unsigned s_trace_estimates_count;
static float s_trace_est[4];
static uint8_t s_trace_tones[3];
static uint16_t s_trace_peak_floor_x10;
static float s_trace_stock_ifft;
static const char *s_trace_why;
static void nrf_cs_trace_zephyr_estimates(uint32_t id,float ifft,float phase,float rtt,float best,
    uint8_t planned,uint8_t usable,uint8_t gap,uint8_t lead,uint8_t tail,uint16_t peak_floor_x10,float stock_ifft,const char *why)
{ (void)id;(void)peak_floor_x10;s_trace_peak_floor_x10=peak_floor_x10;s_trace_stock_ifft=stock_ifft;s_trace_why=why;++s_trace_estimates_count;s_trace_est[0]=ifft;s_trace_est[1]=phase;s_trace_est[2]=rtt;s_trace_est[3]=best;
  s_trace_tones[0]=planned;s_trace_tones[1]=usable;s_trace_tones[2]=gap;(void)lead;(void)tail; }
static void nrf_cs_trace_zephyr_rf(uint32_t id,uint32_t signal,uint32_t quality)
{ (void)id;++s_trace_rf_count;s_trace_rf_signal=signal;s_trace_rf_quality=quality; }
#include "runtime_emit_rf_diagnostics.inc"
#include "runtime_adapter_complete.inc"
#include "report_ranging_header.inc"
#include "accumulate_mean.inc"
#include "collect_pbr_step.inc"
#include "collect_rtt_step.inc"
#include "report_step_data.inc"
#include "current_tone_stats.inc"
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
        if(s_de_scenario==8 && i==9)continue;
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
    if(s_de_scenario==6 || s_de_scenario==7){report->distance_estimates[0].ifft=NAN;report->distance_estimates[0].phase_slope=NAN;report->distance_estimates[0].best=NAN;}
    if(s_de_scenario==7)report->distance_estimates[0].rtt=NAN;
    if(s_de_scenario==9)report->distance_estimates[0].rtt=NAN;
    if(s_de_scenario==10)report->distance_estimates[0].rtt=0.0f;
    if(s_de_scenario==11){report->distance_estimates[0].ifft=NAN;report->distance_estimates[0].phase_slope=NAN;report->distance_estimates[0].best=NAN;report->distance_estimates[0].rtt=0.0f;}
    if(s_de_scenario==12){report->distance_estimates[0].ifft=NAN;report->distance_estimates[0].phase_slope=NAN;report->distance_estimates[0].best=0.0f;report->distance_estimates[0].rtt=0.0f;}
    return CS_DE_QUALITY_OK;
}
static void cs_de_combined_iq_calculate(void *tones,float *iq)
{ (void)tones;for(unsigned i=0;i<75;i++){iq[2*i]=s_iq_counts[i]?1.0f:0.0f;iq[2*i+1]=0.0f;} }
static float s_probe_profile_peak=3.0f;       /* not sharp unless a scenario says so */
static float cs_de_ifft(float *iq)
{
    assert(iq[14]==1.0f && iq[15]==0.0f);
    /* Like the library, leave the IFFT magnitude profile in the first NFFT floats of the buffer. */
    for(unsigned i=0;i<CONFIG_BT_CS_DE_NFFT_SIZE;i++)iq[i]=1.0f;
    iq[100]=s_probe_profile_peak;
    return 1.5f;
}
#include "estimate_measurement.inc"
static void cleanup_work_handler(struct k_work *work);
#include "op_cleanup.inc"
#include "timeout_work_handler.inc"
#include "cleanup_work_handler.inc"
#include "maybe_complete_measurement.inc"
#include "event_work_handler.inc"
#include "execute_work_handler.inc"
#include "nrf_cs_pair_runtime_zephyr_execute.inc"
#include "nrf_cs_pair_runtime_zephyr_set_radio_enabled.inc"
#include "procedure_enable_complete_cb.inc"
#include "nrf_cs_pair_runtime_zephyr_is_progress_required.inc"
#include "nrf_cs_pair_runtime_zephyr_is_busy.inc"
#include "subevent_data_cb.inc"

static int connect_op(void *c,nrf_cs_node_id_t id)
{ (void)c;(void)id;s_now+=s_connect_delay;return 0; }
static int secure_op(void *c) { (void)c;++s_secure_calls;return 0; }
static int noop(void *c) { (void)c;return 0; }
static int config_op(void *c,bool realtime) { (void)realtime;return noop(c); }
static int cleanup_op(void *c) { assert(s_cancel_lock_depth==0);++s_cleanup_calls;
    return s_use_production_cleanup?op_cleanup(c):0; }
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
static void test_procedure_reset_keeps_prepared_connection_state(void)
{
    start(0);advance_to_procedure();
    memset(&s_cs_config,0xa5,sizeof(s_cs_config));s_realtime_ras=true;
    s_disconnect_retry_pending=1;s_reflector_procedure_active=1;
    s_local_steps.len=s_peer_rx.len=s_peer_owned.len=2;
    s_local_counter=7U;s_peer_counter=7U;s_ready_counter=7U;
    s_local_complete=s_peer_complete=s_on_demand_request_started=true;
    s_local_data_frozen=s_peer_data_frozen=1;s_rf_flags=NRF_CS_RF_ABORTED;
    runtime_reset_procedure(8U);
    printf("procedure reset config=%u realtime=%u retry=%u\n",s_cs_config.channel_map[0],s_realtime_ras,s_disconnect_retry_pending);fflush(stdout);
    assert(s_cs_config.channel_map[0]==0xa5 && s_realtime_ras && s_disconnect_retry_pending==1);
    assert(s_reflector_procedure_active==1);
    assert(s_local_steps.len==0 && s_peer_rx.len==0 && s_peer_owned.len==0);
    assert(!s_local_complete && !s_peer_complete && !s_on_demand_request_started);
    assert(s_local_counter==NRF_CS_RANGING_COUNTER_NONE && s_peer_counter==NRF_CS_RANGING_COUNTER_NONE && s_ready_counter==NRF_CS_RANGING_COUNTER_NONE);
    assert(s_local_data_frozen==0 && s_peer_data_frozen==0 && s_rf_flags==0);
    assert(s_expected_counter==8U);
    runtime_reset_ranging();
    assert(s_cs_config.channel_map[0]==0 && !s_realtime_ras && s_disconnect_retry_pending==0);
    assert(s_reflector_procedure_active==0 && s_expected_counter==NRF_CS_RANGING_COUNTER_NONE);
}

static void test_expected_procedure_counter_filters_before_iq_and_rf_updates(void)
{
    const uint16_t counters[]={8U,4096U,65535U,0U};
    for(unsigned mode=0U;mode<2U;++mode)for(unsigned index=0U;index<4U;++index)for(unsigned order=0U;order<2U;++order){
        start(0);advance_to_procedure();s_now=100;
        memset(&s_cs_config,0xa5,sizeof(s_cs_config));s_realtime_ras=mode!=0U;
        const uint16_t expected=bt_ras_rreq_get_ranging_counter(counters[index]);
        runtime_reset_procedure(expected);s_count=s_taken=0;s_capture_posts=true;
        struct net_buf_simple data={.len=2,.data={0x11,0x22}};
        struct bt_conn_le_cs_subevent_result radio={.header={.num_antenna_paths=2,
            .procedure_done_status=BT_CONN_LE_CS_PROCEDURE_ABORTED},.step_data_buf=&data};
        s_peer_rx=(struct net_buf_simple){.len=2,.data={0xaa,0xbb}};
        for(unsigned bad=0U;bad<2U;++bad){
            const uint16_t wrong=(uint16_t)((expected+(bad?1U:4095U))&0xfffU);
            radio.header.procedure_counter=wrong;
            subevent_data_cb(s_outgoing_conn,&radio);
            ranging_data_ready_cb(s_outgoing_conn,wrong);
            ranging_data_received_cb(s_outgoing_conn,wrong,-EIO);
            ranging_data_received_cb(s_outgoing_conn,wrong,0);
        }
        assert(s_count==0 && s_local_steps.len==0 && s_peer_owned.len==0 && s_rf_flags==0);
        assert(!s_local_data_frozen && !s_peer_data_frozen && s_ready_counter==NRF_CS_RANGING_COUNTER_NONE);
        radio.header.num_antenna_paths=1;radio.header.procedure_done_status=BT_CONN_LE_CS_PROCEDURE_COMPLETE;
        radio.header.procedure_counter=counters[index];s_peer_rx.data[0]=0x33;
        ranging_data_ready_cb(s_outgoing_conn,expected);
        if(order==0U)subevent_data_cb(s_outgoing_conn,&radio);
        ranging_data_received_cb(s_outgoing_conn,expected,0);
        if(order==1U)subevent_data_cb(s_outgoing_conn,&radio);
        assert(s_count==3 && s_local_steps.data[0]==0x11 && s_peer_owned.data[0]==0x33);
        assert(s_cs_config.channel_map[0]==0xa5 && s_realtime_ras==(mode!=0U));
        s_capture_posts=false;event_work_handler(NULL);
        assert(s_local_counter==expected && s_peer_counter==expected && s_ready_counter==expected);
        assert(s_adapter.phase==NRF_CS_PAIR_PHASE_CLEANUP && s_callbacks==0);
        cleanup_work_handler(NULL);
        assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_SUCCESS);
        assert(s_expected_counter==NRF_CS_RANGING_COUNTER_NONE && s_cs_config.channel_map[0]==0);
        printf("expected IQ full=%u ras=%u order=%u realtime=%u accepted=1\n",counters[index],expected,order,mode);
    }
}

static void test_completed_iq_buffers_survive_queued_callbacks(void)
{
    const uint16_t counters[]={7U,4095U,0U};
    for(unsigned i=0;i<6;++i){
        const uint16_t counter=counters[i%3U];
        start(0);advance_to_procedure();s_now=100;s_count=s_taken=0;s_capture_posts=true;
        struct net_buf_simple data={.len=2,.data={0x11,0x22}};
        struct bt_conn_le_cs_subevent_result radio={.header={.num_antenna_paths=1,
            .procedure_done_status=BT_CONN_LE_CS_PROCEDURE_COMPLETE,.procedure_counter=counter},
            .step_data_buf=&data};
        s_peer_rx=(struct net_buf_simple){.len=2,.data={0x33,0x44}};
        if(i<3U)subevent_data_cb(s_outgoing_conn,&radio);
        ranging_data_received_cb(s_outgoing_conn,counter,0);
        if(i>=3U)subevent_data_cb(s_outgoing_conn,&radio);
        /* Both completion events wait while further callbacks reuse the same connection. */
        data.data[0]=0xaa;
        subevent_data_cb(s_outgoing_conn,&radio); /* Duplicate completed procedure. */
        radio.header.procedure_counter=(uint16_t)((counter+1U)&0xfffU);
        subevent_data_cb(s_outgoing_conn,&radio);
        s_peer_rx.data[0]=0xbb;
        ranging_data_received_cb(s_outgoing_conn,counter,0);
        ranging_data_received_cb(s_outgoing_conn,radio.header.procedure_counter,0);
        ranging_data_received_cb(s_outgoing_conn,counter,-EIO);
        printf("queued IQ counter=%u local_len=%u local0=%u peer0=%u events=%u\n",
            counter,s_local_steps.len,s_local_steps.data[0],s_peer_owned.data[0],s_count);
        fflush(stdout);
        assert(s_local_steps.len==2 && s_local_steps.data[0]==0x11);
        assert(s_peer_owned.len==2 && s_peer_owned.data[0]==0x33);
        assert(s_count==2);
        s_capture_posts=false;
        event_work_handler(NULL);cleanup_work_handler(NULL);
        assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_SUCCESS);
        runtime_reset_ranging();
        assert(s_local_counter==NRF_CS_RANGING_COUNTER_NONE && s_peer_counter==NRF_CS_RANGING_COUNTER_NONE);
    }
}
static void test_iq_partial_abort_overflow_and_mismatch(void)
{
    for(unsigned scenario=0;scenario<3;++scenario){
        start(0);advance_to_procedure();s_now=100;s_count=s_taken=0;s_capture_posts=true;
        struct net_buf_simple data={.len=1,.data={0x11}};
        struct bt_conn_le_cs_subevent_result radio={.header={.num_antenna_paths=1,
            .procedure_done_status=1,.procedure_counter=4095U},.step_data_buf=&data};
        subevent_data_cb(s_outgoing_conn,&radio);
        assert(s_local_steps.len==1 && s_count==0);
        radio.header.procedure_counter=0;data.data[0]=0x22;
        subevent_data_cb(s_outgoing_conn,&radio);
        assert(s_local_steps.len==1 && s_local_steps.data[0]==0x22 && s_count==0);
        radio.header.procedure_done_status=scenario==1U?BT_CONN_LE_CS_PROCEDURE_ABORTED:BT_CONN_LE_CS_PROCEDURE_COMPLETE;
        if(scenario==2U)s_local_steps.len=128; /* No space for the next valid byte. */
        data.data[0]=0x33;subevent_data_cb(s_outgoing_conn,&radio);
        assert(s_count==1);
        if(scenario==0U)assert(s_local_steps.len==2 && s_local_steps.data[0]==0x22 && s_local_steps.data[1]==0x33);
        radio.header.procedure_done_status=BT_CONN_LE_CS_PROCEDURE_COMPLETE;
        subevent_data_cb(s_outgoing_conn,&radio);
        assert(s_count==1);
        if(scenario==0U){
            s_peer_rx=(struct net_buf_simple){.len=1,.data={0x44}};
            ranging_data_received_cb(s_outgoing_conn,4095U,0); /* Different from completed local counter0. */
            assert(s_count==2);
        }
        s_capture_posts=false;event_work_handler(NULL);cleanup_work_handler(NULL);
        assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_OUTLIER && s_de_calls==0 && s_confidence==0);
    }
}
static void test_radio_stop_closes_owned_reflector_on_worker(void)
{
    for(unsigned scenario=0;scenario<5;++scenario){
        struct bt_conn replacement={.refs=1};
        start(0);advance_to_procedure();timeout_work_handler(NULL);cleanup_work_handler(NULL);
        s_outgoing_conn=NULL;s_connection.refs=1;s_incoming_conn=&s_connection;
        s_count=s_taken=0;s_capture_posts=true;s_radio_enabled=1;
        s_advertising=true;s_controller_advertising=true;
        s_disconnect_calls=s_stop_advertising_calls=s_reflector_config_calls=0;
        if(scenario==3U){
            runtime_post_event(RUNTIME_EVENT_INCOMING_CONNECTED,NULL,0,0,s_incoming_conn);
            runtime_post_event(RUNTIME_EVENT_REFLECTOR_CONFIG,NULL,0,0,s_incoming_conn);
        }
        if(scenario==4U)s_incoming_conn=NULL;
        assert(nrf_cs_pair_runtime_zephyr_set_radio_enabled(false)==0);
        printf("radio stop scenario=%u queued=%u disconnect_inline=%u\n",scenario,s_count,s_disconnect_calls);fflush(stdout);
        if(scenario==0U)assert(s_count==1 && s_disconnect_calls==0 && s_stop_advertising_calls==1);
        if(scenario==1U)assert(nrf_cs_pair_runtime_zephyr_set_radio_enabled(true)==0);
        if(scenario==2U)s_incoming_conn=&replacement;
        s_capture_posts=false;event_work_handler(NULL);
        if(scenario==0U)assert(s_disconnect_calls==1 && s_disconnected_conn==&s_connection);
        else if(scenario==3U)assert(s_disconnect_calls==3 && s_reflector_config_calls==0);
        else assert(s_disconnect_calls==0);
        assert(s_connection.refs==1 && replacement.refs==1);
        if(scenario==0U || scenario==3U){
            s_count=s_taken=0;s_capture_posts=true;
            disconnected_cb(&s_connection,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            assert(s_incoming_conn==NULL && s_connection.refs==0);
            assert(nrf_cs_pair_runtime_zephyr_is_progress_required());
            s_capture_posts=false;event_work_handler(NULL);
            assert(!nrf_cs_pair_runtime_zephyr_is_progress_required());
        } else bt_conn_unref(&s_connection);
        s_incoming_conn=NULL;s_radio_enabled=1;
    }
}
static void test_failed_advertising_stop_keeps_role_before_scan(void)
{
    const int faults[]={-EIO,-ENOMEM};
    for(unsigned fault=0U;fault<2U;++fault){
    start(0);s_outgoing_conn=NULL;s_incoming_conn=NULL;s_scanning=false;
    s_advertising=s_controller_advertising=true;s_advertising_stop_result=faults[fault];s_scan_starts=0;
    s_local_steps.len=1;s_scan_target=99U;
    const int result=op_connect(NULL,2U);
    printf("stop failure result=%d advertising=%u controller=%u scans=%u\n",result,
        s_advertising,s_controller_advertising,s_scan_starts);fflush(stdout);
    assert(result==faults[fault] && s_advertising && s_controller_advertising && !s_scanning && s_scan_starts==0);
    assert(s_local_steps.len==1 && s_scan_target==99U);
    s_advertising_stop_result=fault==0U?0:-EALREADY;
    assert(op_connect(NULL,2U)==0);
    assert(!s_advertising && !s_controller_advertising && s_scanning && s_scan_starts==1);
    assert(s_local_steps.len==0 && s_scan_target==2U);
    const unsigned stops=s_stop_advertising_calls;
    assert(runtime_stop_advertising()==0 && s_stop_advertising_calls==stops);
    s_scanning=false;
    }
    s_advertising_stop_result=0;
}

static void test_outgoing_cancellation_is_queued_and_token_scoped(void)
{
    for(unsigned phase=0;phase<4;++phase){
        start(0);
        if(phase==1U)deliver(RUNTIME_EVENT_CONNECTED,&s_timeout_step);
        if(phase>=2U)advance_to_procedure();
        if(phase==3U){
            s_local_steps.len=s_peer_owned.len=1;s_now=100;
            deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
        }
        const unsigned before=s_cleanup_calls;s_now=100;
        const unsigned submits=s_event_submits;
        nrf_cs_pair_runtime_zephyr_cancel(&s_timeout_step);
        assert(s_event_submits==submits+1);
        assert(s_cleanup_calls==before && s_callbacks==0);
        event_work_handler(NULL);
        printf("cancel phase=%u cleanup=%u callbacks=%u\n",phase,s_cleanup_calls,s_callbacks);fflush(stdout);
        assert(s_cleanup_calls==1 && nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_CLEANUP);
        s_now=120;cleanup_work_handler(NULL);
        assert(s_callbacks==1 && s_outcome==(phase==3U?NRF_CS_OUTCOME_SUCCESS:NRF_CS_OUTCOME_TRANSPORT_ERROR));
        event_work_handler(NULL);cleanup_work_handler(NULL);assert(s_callbacks==1);
    }
    for(unsigned field=0;field<7;++field){
        start(0);advance_to_procedure();nrf_cs_step_t stale=s_timeout_step;
        switch(field){case 0:++stale.cycle_id;break;case 1:++stale.membership_generation;break;
            case 2:++stale.attempt_id;break;case 3:++stale.pair_index;break;
            case 4:++stale.attempt_number;break;case 5:++stale.initiator;break;default:++stale.reflector;break;}
        nrf_cs_pair_runtime_zephyr_cancel(&stale);event_work_handler(NULL);
        assert(s_cleanup_calls==0 && nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_PROCEDURE);
        nrf_cs_pair_runtime_zephyr_cancel(&s_timeout_step);
        nrf_cs_pair_runtime_zephyr_cancel(&stale); /* A late request cannot overwrite a valid pending cancel. */
        event_work_handler(NULL);cleanup_work_handler(NULL);
        assert(s_callbacks==1);
    }
    /* Cancellation can precede adapter_execute, including the take_start/execute handoff. */
    for(unsigned before_accept=0;before_accept<2;++before_accept){
        start(0);timeout_work_handler(NULL);cleanup_work_handler(NULL);
        nrf_cs_step_t next=s_timeout_step;++next.attempt_id;
        if(before_accept)nrf_cs_pair_runtime_zephyr_cancel(&next);
        assert(nrf_cs_pair_runtime_zephyr_execute(&next,completed,NULL)==0);
        if(!before_accept)nrf_cs_pair_runtime_zephyr_cancel(&next);
        event_work_handler(NULL); /* Adapter idle: preserve cancellation for queued execute. */
        const unsigned cleanups=s_cleanup_calls;
        execute_work_handler(NULL);
        assert(s_callbacks==2 && s_outcome==NRF_CS_OUTCOME_TRANSPORT_ERROR && s_cleanup_calls==cleanups);
        assert(!nrf_cs_pair_adapter_is_active(&s_adapter) && s_execute_pending==0);
    }
}
static void test_disconnect_failure_keeps_live_connection_owned(void)
{
    for(unsigned fault=0;fault<3;++fault){
    start(0);advance_to_procedure();s_connection.refs=1;s_use_production_cleanup=true;
    s_disconnect_result=fault==0U?-ENOMEM:-EIO;s_disconnect_calls=0;s_now=100;
    s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    printf("disconnect error owned=%u refs=%u callbacks=%u\n",s_outgoing_conn==&s_connection,
        s_connection.refs,s_callbacks);fflush(stdout);
    assert(s_outgoing_conn==&s_connection && s_connection.refs==1 && s_callbacks==0);
    if(fault!=2U)s_disconnect_result=0;
    s_now=500;timeout_work_handler(NULL);
    assert(s_disconnect_calls==2 && s_outgoing_conn==&s_connection && s_callbacks==0);
    if(fault==2U){
        nrf_cs_step_t next=s_timeout_step;++next.attempt_id;
        assert(nrf_cs_pair_runtime_zephyr_execute(&next,completed,NULL)==-EBUSY);
    }
    s_capture_posts=true;disconnected_cb(&s_connection,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    s_capture_posts=false;event_work_handler(NULL);
    assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_TRANSPORT_ERROR && s_connection.refs==0);
    s_use_production_cleanup=false;s_disconnect_result=0;
    }
}
static void test_lost_disconnect_event_does_not_leave_cleanup_active(void)
{
    for(unsigned outcome=0;outcome<3;++outcome)for(unsigned after_deadline=0;after_deadline<2;++after_deadline){
    start(0);advance_to_procedure();s_connection.refs=1;s_use_production_cleanup=true;
    s_disconnect_result=0;s_now=100;s_local_steps.len=s_peer_owned.len=1;
    const nrf_cs_outcome_t expected=outcome==0U?NRF_CS_OUTCOME_SUCCESS:
        outcome==1U?NRF_CS_OUTCOME_TRANSPORT_ERROR:NRF_CS_OUTCOME_TIMEOUT;
    if(outcome==0U){deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);}
    else if(outcome==1U)deliver(RUNTIME_EVENT_TRANSPORT_ERROR,&s_timeout_step);
    else {s_now=500;timeout_work_handler(NULL);}
    assert(nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_CLEANUP);
    if(after_deadline){s_now=500;timeout_work_handler(NULL);assert(s_callbacks==0);}
    s_count=s_taken=0;s_capture_posts=true;
    nrf_cs_step_t foreign=s_timeout_step;++foreign.attempt_id;
    for(unsigned i=0;i<NRF_CS_EVENT_QUEUE_DEPTH;++i)
        runtime_post_event(RUNTIME_EVENT_CONNECTED,&foreign,0U,0,NULL);
    const uint32_t overflows=s_event_queue_overflow_total;
    disconnected_cb(&s_connection,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    assert(s_event_queue_overflow_total==overflows+1 && s_connection.refs==0 && s_outgoing_conn==NULL);
    s_capture_posts=false;
    if(after_deadline)event_work_handler(NULL);
    else {s_now=500;timeout_work_handler(NULL);}
    printf("lost disconnect callbacks=%u active=%u\n",s_callbacks,nrf_cs_pair_adapter_is_active(&s_adapter));fflush(stdout);
    assert(s_callbacks==1 && !nrf_cs_pair_adapter_is_active(&s_adapter));
    assert(s_outcome==expected && s_connection.refs==0);
    event_work_handler(NULL);cleanup_work_handler(NULL);timeout_work_handler(NULL);
    assert(s_callbacks==1);
    s_use_production_cleanup=false;
    const nrf_cs_step_t old=s_timeout_step;
    nrf_cs_step_t next=old;++next.attempt_id;
    s_outgoing_conn=&s_connection;s_connection.refs=1;
    assert(nrf_cs_pair_runtime_zephyr_execute(&next,completed,NULL)==0);
    execute_work_handler(NULL);
    deliver(RUNTIME_EVENT_DISCONNECTED,&old);
    assert(s_callbacks==1 && nrf_cs_pair_adapter_phase(&s_adapter)==NRF_CS_PAIR_PHASE_CONNECTING);
    advance_to_procedure();s_now+=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_callbacks==2 && s_outcome==NRF_CS_OUTCOME_SUCCESS);
    }
}

static void test_cleanup_submit_failure_has_no_connection_to_wait_for(void)
{
    for(unsigned owned=0;owned<2;++owned){
        start(0);advance_to_procedure();s_connection.refs=1;
        if(!owned)s_outgoing_conn=NULL;
        s_use_production_cleanup=true;s_failed_submit=&s_cleanup_work;s_disconnect_result=-ENOTCONN;
        s_now=100;s_local_steps.len=s_peer_owned.len=1;
        const nrf_cs_measurement_t measurement={.outcome=NRF_CS_OUTCOME_SUCCESS,.distance_mm=1000,.confidence=900};
        (void)nrf_cs_pair_adapter_measurement_ready(&s_adapter,&s_timeout_step,&measurement);
        assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_SUCCESS && !nrf_cs_pair_adapter_is_active(&s_adapter));
        assert(s_outgoing_conn==NULL && s_connection.refs==(owned?0U:1U));
        s_failed_submit=NULL;s_use_production_cleanup=false;s_disconnect_result=0;
    }
}
static void test_disconnect_already_pending_waits_for_real_callback(void)
{
    start(0);advance_to_procedure();s_connection.refs=1;s_use_production_cleanup=true;
    s_disconnect_result=-EALREADY;s_disconnect_calls=0;s_now=100;
    s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    assert(s_outgoing_conn==&s_connection && s_connection.refs==1 && s_callbacks==0);
    s_now=500;timeout_work_handler(NULL);
    assert(s_disconnect_calls==1 && s_callbacks==0);
    s_capture_posts=true;disconnected_cb(&s_connection,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    s_capture_posts=false;event_work_handler(NULL);
    assert(s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_SUCCESS && s_connection.refs==0);
    s_use_production_cleanup=false;s_disconnect_result=0;
}
static void test_release_reason_reaches_reflector_only_for_final_execution(void)
{
    for(unsigned scenario=0;scenario<3;++scenario){
        start(0);advance_to_procedure();s_connection.refs=1;s_use_production_cleanup=true;
        s_relay_final_hint_cb=final_hint;s_final_hint=scenario==0U;s_disconnect_result=0;s_now=100;
        if(scenario==2U){nrf_cs_pair_runtime_zephyr_cancel(&s_timeout_step);event_work_handler(NULL);}
        else {s_local_steps.len=s_peer_owned.len=1;
            deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);}
        const uint8_t reason=s_last_disconnect_reason;
        assert(reason==(s_final_hint?NRF_CS_CYCLE_RELEASE_REASON:BT_HCI_ERR_REMOTE_USER_TERM_CONN));
        s_capture_posts=true;disconnected_cb(&s_connection,reason);s_capture_posts=false;event_work_handler(NULL);
        assert(s_callbacks==1 && s_connection.refs==0);
        struct bt_conn peer={.refs=1};s_incoming_conn=&peer;s_reflector_procedure_active=1;
        s_relay_released_cb=released_hint;s_release_notifications=0;
        s_release_cycle=(nrf_cs_cycle_runtime_t){.state=NRF_CS_CYCLE_STATE_RUNNING,.relay=true,
            .local_node=1U,.step_count=2U,.window=0U};
        s_release_cycle.steps[0].reflector=1U;s_release_cycle.steps[1].reflector=2U;
        s_capture_posts=true;disconnected_cb(&peer,reason);s_capture_posts=false;event_work_handler(NULL);
        assert(s_release_notifications==(scenario==0U?1U:0U) && peer.refs==0);
        assert(!nrf_cs_pair_runtime_zephyr_is_progress_required());
        s_use_production_cleanup=false;s_relay_final_hint_cb=NULL;s_relay_released_cb=NULL;
    }
}
static void test_full_queue_does_not_lose_incoming_release(void)
{
    const uint8_t reasons[]={NRF_CS_CYCLE_RELEASE_REASON,BT_HCI_ERR_REMOTE_USER_TERM_CONN,0x09U};
    for(unsigned i=0;i<6;++i){
        start(0);deliver(RUNTIME_EVENT_TRANSPORT_ERROR,&s_timeout_step);cleanup_work_handler(NULL);
        s_outgoing_conn=NULL;s_count=s_taken=0;s_capture_posts=true;
        struct bt_conn peer={.refs=1};s_incoming_conn=&peer;s_reflector_procedure_active=1;
        s_relay_released_cb=released_hint;s_release_notifications=0;
        const unsigned starts=s_advertising_starts;
        s_release_cycle=(nrf_cs_cycle_runtime_t){.state=NRF_CS_CYCLE_STATE_RUNNING,.relay=true,
            .local_node=1U,.step_count=2U,.window=0U};
        s_release_cycle.steps[0].reflector=1U;s_release_cycle.steps[1].reflector=2U;
        for(unsigned n=0;n<NRF_CS_EVENT_QUEUE_DEPTH;++n)
            runtime_post_event(RUNTIME_EVENT_CONNECTED,&s_timeout_step,0U,0,NULL);
        const uint32_t overflows=s_event_queue_overflow_total;
        if(i>=3U)s_failed_submit=&s_event_work;
        disconnected_cb(&peer,reasons[i%3U]);
        assert(peer.refs==0 && s_incoming_conn==NULL && s_event_queue_overflow_total==overflows+1);
        assert(s_release_notifications==0 && s_advertising_starts==starts);
        assert(nrf_cs_pair_runtime_zephyr_is_busy() && nrf_cs_pair_runtime_zephyr_is_progress_required());
        assert(nrf_cs_pair_runtime_zephyr_execute(&s_timeout_step,completed,NULL)==-EBUSY);
        struct bt_conn premature={.refs=1};const unsigned disconnects=s_disconnect_calls;
        connected_cb(&premature,0);
        assert(s_incoming_conn==NULL && premature.refs==1 && s_disconnect_calls==disconnects+1);
        s_failed_submit=NULL;s_capture_posts=false;event_work_handler(NULL);
        printf("incoming lost reason=%u releases=%u advertising=%u\n",reasons[i%3U],s_release_notifications,s_advertising_starts-starts);fflush(stdout);
        assert(s_release_notifications==(i%3U==0U?1U:0U) && s_advertising_starts==starts+1);
        assert(s_release_cycle.released_mask==(i%3U==0U?1U:0U));
        assert(!nrf_cs_pair_runtime_zephyr_is_busy() && !nrf_cs_pair_runtime_zephyr_is_progress_required());
        disconnected_cb(&peer,reasons[i%3U]);event_work_handler(NULL);
        assert(s_release_notifications==(i%3U==0U?1U:0U) && s_advertising_starts==starts+1);
        s_relay_released_cb=NULL;
    }
}

static void test_worker_cannot_deliver_before_incoming_ownership_is_released(void)
{
    start(0);deliver(RUNTIME_EVENT_TRANSPORT_ERROR,&s_timeout_step);cleanup_work_handler(NULL);
    s_outgoing_conn=NULL;s_count=s_taken=0;s_capture_posts=true;
    struct bt_conn peer={.refs=1};s_incoming_conn=&peer;s_reflector_procedure_active=1;
    s_relay_released_cb=released_hint;s_release_notifications=0;
    s_worker_during_incoming_unref=true;
    disconnected_cb(&peer,NRF_CS_CYCLE_RELEASE_REASON);
    assert(s_release_notifications==0 && peer.refs==0 && s_incoming_conn==NULL);
    s_capture_posts=false;event_work_handler(NULL);
    assert(s_release_notifications==1 && !nrf_cs_pair_runtime_zephyr_is_busy());
    event_work_handler(NULL);assert(s_release_notifications==1);
    s_relay_released_cb=NULL;
}

static void test_failed_cancel_submit_retains_request_or_deadline_fallback(void)
{
    for(unsigned via_timeout=0;via_timeout<2;++via_timeout){
        start(0);advance_to_procedure();s_failed_submit=&s_event_work;
        nrf_cs_pair_runtime_zephyr_cancel(&s_timeout_step);
        assert(s_cancel_pending && s_cleanup_calls==0 && s_callbacks==0);
        s_failed_submit=NULL;s_now=via_timeout?500U:100U;
        if(via_timeout)timeout_work_handler(NULL);else event_work_handler(NULL);
        cleanup_work_handler(NULL);
        assert(s_callbacks==1 && s_outcome==(via_timeout?NRF_CS_OUTCOME_TIMEOUT:NRF_CS_OUTCOME_TRANSPORT_ERROR));
        nrf_cs_step_t next=s_timeout_step;++next.attempt_id;
        assert(nrf_cs_pair_runtime_zephyr_execute(&next,completed,NULL)==0);
        assert(!s_cancel_pending);execute_work_handler(NULL);advance_to_procedure();
        s_now+=100;s_local_steps.len=s_peer_owned.len=1;
        deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
        cleanup_work_handler(NULL);
        assert(s_callbacks==2 && s_outcome==NRF_CS_OUTCOME_SUCCESS);
    }
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
static void test_consecutive_attempts_do_not_reuse_cleanup_timing(void)
{
    start(0);advance_to_procedure();s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    s_now=500;timeout_work_handler(NULL);s_now=700;cleanup_work_handler(NULL);
    assert(s_callbacks==1 && s_trace_result_timing.cleanup_ms==600 && s_trace_result_timing.timeout_during_cleanup);
    for(unsigned failure=0;failure<2;++failure) {
        nrf_cs_step_t next=s_timeout_step;
        ++next.attempt_id;
        const unsigned elapsed=failure==0?500:20;
        s_schedule_fail=failure!=0;
        assert(nrf_cs_pair_runtime_zephyr_execute(&next,completed,NULL)==0);
        s_now+=elapsed;
        execute_work_handler(NULL);
        printf("new queued attempt id=%u measured=%u cleanup=%u total=%u cleanup_timeout=%u\n",
            (unsigned)next.attempt_id,(unsigned)s_trace_result_timing.measured_ms,
            (unsigned)s_trace_result_timing.cleanup_ms,(unsigned)s_trace_result_timing.total_ms,
            s_trace_result_timing.timeout_during_cleanup?1U:0U);
        fflush(stdout);
        assert(s_callbacks==2+failure && s_cleanup_calls==1);
        assert(s_trace_result_attempt==next.attempt_id);
        assert(s_outcome==(failure==0?NRF_CS_OUTCOME_TIMEOUT:NRF_CS_OUTCOME_TRANSPORT_ERROR));
        assert(s_trace_result_has_timing && s_trace_result_timing.valid);
        assert(s_trace_result_timing.cleanup_ms==0 && !s_trace_result_timing.timeout_during_cleanup);
        assert(s_trace_result_timing.measured_ms==elapsed && s_trace_result_timing.total_ms==elapsed);
        assert(!s_trace_result_timing.budget_exceeded && !nrf_cs_pair_adapter_is_active(&s_adapter));
    }
    s_schedule_fail=false;
}

static void test_procedure_disabled_is_not_an_enabled_reflector(void)
{
    s_incoming_conn=&s_connection;s_outgoing_conn=NULL;s_reflector_procedure_active=0;s_rf_flags=0;
    struct bt_conn_le_cs_procedure_enable_complete enabled={.state=BT_CONN_LE_CS_PROCEDURES_ENABLED,
        .selected_tx_power=4,.tone_antenna_config_selection=BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1};
    procedure_enable_complete_cb(s_incoming_conn,0,&enabled);
    assert(s_reflector_procedure_active==1 && s_rf_tx_power==4 && nrf_cs_pair_runtime_zephyr_is_progress_required());
    struct bt_conn_le_cs_procedure_enable_complete disabled={.state=BT_CONN_LE_CS_PROCEDURES_DISABLED,
        .selected_tx_power=127,.tone_antenna_config_selection=99};
    procedure_enable_complete_cb(s_incoming_conn,0,&disabled);
    printf("disabled reflector callback active=%d tx=%d flags=%d\n",s_reflector_procedure_active,s_rf_tx_power,s_rf_flags);
    fflush(stdout);
    assert(s_reflector_procedure_active==0);
    assert(!nrf_cs_pair_runtime_zephyr_is_progress_required());
    assert(s_rf_tx_power==4 && s_rf_flags==0);
    procedure_enable_complete_cb(s_incoming_conn,0,NULL);
    assert(s_reflector_procedure_active==0);
    procedure_enable_complete_cb(s_incoming_conn,9,&enabled);
    assert(s_reflector_procedure_active==0);
    s_incoming_conn=NULL;
}

int main(void)
{
    test_procedure_reset_keeps_prepared_connection_state();
    test_expected_procedure_counter_filters_before_iq_and_rf_updates();
    test_failed_advertising_stop_keeps_role_before_scan();
    test_completed_iq_buffers_survive_queued_callbacks();
    test_iq_partial_abort_overflow_and_mismatch();
    test_radio_stop_closes_owned_reflector_on_worker();
    test_outgoing_cancellation_is_queued_and_token_scoped();
    test_cleanup_submit_failure_has_no_connection_to_wait_for();
    test_lost_disconnect_event_does_not_leave_cleanup_active();
    test_full_queue_does_not_lose_incoming_release();
    test_worker_cannot_deliver_before_incoming_ownership_is_released();
    test_disconnect_already_pending_waits_for_real_callback();
    test_disconnect_failure_keeps_live_connection_owned();
    test_release_reason_reaches_reflector_only_for_final_execution();
    test_failed_cancel_submit_retains_request_or_deadline_fallback();
    test_procedure_disabled_is_not_an_enabled_reflector();
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
    const struct bt_conn_le_cs_procedure_enable_complete power={.state=BT_CONN_LE_CS_PROCEDURES_ENABLED,.selected_tx_power=4,
        .tone_antenna_config_selection=BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1};
    procedure_enable_complete_cb(s_outgoing_conn,0,(void *)&power);
    const struct bt_conn_le_cs_procedure_enable_complete disabled_power={.state=BT_CONN_LE_CS_PROCEDURES_DISABLED,
        .selected_tx_power=127,.tone_antenna_config_selection=99};
    procedure_enable_complete_cb(s_outgoing_conn,0,(void *)&disabled_power);
    assert(s_rf_tx_power==4 && s_rf_flags==0);
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
    /* NCS 3.4: bad tone quality skips set_best_estimate even when RTT is finite. */
    start(0);advance_to_procedure();s_ras_skip=true;s_de_scenario=6;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_confidence==400);
    assert((s_provider_flags & 0x70U)==NRF_CS_PROVIDER_METHOD_RTT && s_trace_est[3]==1.0f);
    start(0);advance_to_procedure();s_ras_skip=true;s_de_scenario=7;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && (s_provider_flags & 0x70U)==0U);
    /* Production estimator must feed a filled interior gap to Nordic IFFT. */
    start(0);advance_to_procedure();s_de_scenario=8;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_distance==1500 && s_trace_est[0]==1.5f);
    /* The peak sharpness of that IFFT (peak 3 over a floor of 1) reaches the trace as 3.0, in tenths. */
    assert(s_trace_peak_floor_x10==30U);
    /* Both IFFTs are in the log: the library's own (1.0 m, what cs_de_calc returned) and the one with the gap filled (1.5 m, used). */
    assert(s_trace_stock_ifft==1.0f && s_trace_est[0]==1.5f);
    assert(strcmp(s_trace_why,"ok")==0);
    /* Missing RTT must leave the raw PBR result visible but below the production 500 gate (the IFFT peak is not sharp). */
    start(0);advance_to_procedure();s_de_scenario=9;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_distance==1000 && s_confidence<500);
    assert((s_provider_flags & NRF_CS_PROVIDER_QUALITY_RTT_VALID)==0U);
    assert(strcmp(s_trace_why,"rtt_missing")==0);            /* the log says what held the confidence down */
    /* The same answer with a sharp IFFT peak (30 over the floor) is a clean direct path: above the gate, the peak in the trace.
     * Bench 2026-11-03: MAIN->G4 was correct (4.95 and 5.01 m) and rejected for the missing RTT alone. */
    s_probe_profile_peak=30.0f;
    start(0);advance_to_procedure();s_de_scenario=9;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_distance==1000 && s_confidence==600U && s_trace_peak_floor_x10==300U);
    assert(strcmp(s_trace_why,"sharp_peak")==0);
    assert((s_provider_flags & NRF_CS_PROVIDER_QUALITY_RTT_VALID)==0U);
    s_probe_profile_peak=3.0f;
    /* Nordic clamps nonpositive RTT timing to zero; it is not an independent range. */
    start(0);advance_to_procedure();s_de_scenario=10;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_SUCCESS && s_trace_result_distance==1000 && s_confidence<500);
    assert((s_provider_flags & NRF_CS_PROVIDER_QUALITY_RTT_VALID)==0U && s_trace_est[2]==0.0f);
    start(0);advance_to_procedure();s_ras_skip=true;s_de_scenario=11;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && (s_provider_flags & NRF_CS_PROVIDER_QUALITY_RTT_VALID)==0U);
    /* The SDK can select clamped RTT=0 as best even when tone count is nominal. */
    start(0);advance_to_procedure();s_de_scenario=12;
    s_now=100;s_local_steps.len=s_peer_owned.len=1;
    deliver(RUNTIME_EVENT_LOCAL_COMPLETE,&s_timeout_step);deliver(RUNTIME_EVENT_PEER_COMPLETE,&s_timeout_step);
    cleanup_work_handler(NULL);
    assert(s_outcome==NRF_CS_OUTCOME_OUTLIER && (s_provider_flags & NRF_CS_PROVIDER_METHOD_MASK)==0U);
    assert(s_trace_est[2]==0.0f && isnan(s_trace_est[3]));
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
    test_consecutive_attempts_do_not_reuse_cleanup_timing();
    start(0);s_now=60;s_count=1;s_taken=0;                /* an error code of a late event shows in the trace too */
    s_events[0]=(runtime_event_t){.type=RUNTIME_EVENT_TRANSPORT_ERROR,.has_step=true,.step=s_timeout_step,.status=9};
    event_work_handler(NULL);
    assert(s_trace_event_count==2 && !strcmp(s_trace_events[1].label,"TRANSPORT_ERROR") && s_trace_events[1].status==9);
    printf("console trace hooks verified\n");
    printf("PRODUCTION_HANDLER_ACCEPTANCE_GAPS=%u\n",gaps);
    return gaps?1:0;
}
