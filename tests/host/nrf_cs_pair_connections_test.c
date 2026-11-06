#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "nrf_cs_pair_runtime_zephyr_bridge.h"

#define ARG_UNUSED(x) (void)(x)
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
struct bt_conn { unsigned refs; };
static struct bt_conn *s_outgoing_conn, *s_incoming_conn;
static bool s_outgoing_connected, s_advertising;
static int s_reflector_procedure_active;
static int s_radio_enabled=1;
static int s_incoming_disconnect_pending;
static int s_incoming_activity_ms, s_incoming_activity_seen;
static uint32_t s_now_ms;
static uint32_t k_uptime_get_32(void) { return s_now_ms; }
typedef int atomic_val_t;
static nrf_cs_pair_adapter_t s_adapter;
static unsigned s_completions, s_disconnects;
static nrf_cs_outcome_t s_outcome;
typedef enum {
    RUNTIME_EVENT_CONNECTED, RUNTIME_EVENT_TRANSPORT_ERROR,
    RUNTIME_EVENT_DISCONNECTED, RUNTIME_EVENT_INCOMING_CONNECTED,
    RUNTIME_EVENT_INCOMING_DISCONNECTED
} runtime_event_type_t;
static struct bt_conn *bt_conn_ref(struct bt_conn *c) { ++c->refs; return c; }
static void bt_conn_unref(struct bt_conn *c) { assert(c->refs); --c->refs; }
static int bt_conn_disconnect(struct bt_conn *c, uint8_t reason)
{ (void)c; (void)reason; ++s_disconnects; return 0; }
static int op_cleanup(void *context)
{ (void)context; return bt_conn_disconnect(s_outgoing_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); }
static void atomic_set(int *a, int b) { *a = b; }
static int atomic_get(const int *a) { return *a; }
static int runtime_start_advertising(void) { return 0; }
static bool runtime_active_step(nrf_cs_step_t *s)
{
    const nrf_cs_step_t *active = nrf_cs_pair_adapter_active_step(&s_adapter);
    if (!active) return false;
    *s = *active;
    return true;
}
static void runtime_post_event(runtime_event_type_t type, const nrf_cs_step_t *s,
    uint32_t value, int status, struct bt_conn *conn)
{
    (void)value; (void)status; (void)conn;
    if (type == RUNTIME_EVENT_DISCONNECTED || type == RUNTIME_EVENT_TRANSPORT_ERROR) {
        (void)nrf_cs_pair_runtime_bridge_dispatch(&s_adapter,
            type == RUNTIME_EVENT_DISCONNECTED ? NRF_CS_PAIR_BRIDGE_DISCONNECTED :
                NRF_CS_PAIR_BRIDGE_TRANSPORT_ERROR, s, false, NULL);
    }
}
#include "nrf_cs_pair_runtime_zephyr_connections.inc"

static int connect_op(void *c, nrf_cs_node_id_t n) { (void)c; (void)n; return 0; }
static int noop(void *c) { (void)c; return 0; }
static int configure(void *c, bool realtime) { (void)realtime; return noop(c); }
static void complete(const nrf_cs_step_t *s, const nrf_cs_measurement_t *m, void *c)
{ (void)s; (void)c; ++s_completions; s_outcome = m->outcome; }

static nrf_cs_step_t start(struct bt_conn *conn)
{
    const nrf_cs_pair_adapter_ops_t ops = {
        .connect=connect_op, .secure=noop, .exchange_mtu=noop, .discover_ras=noop,
        .read_features=noop, .configure_ranging_data=configure,
        .exchange_capabilities=noop, .create_config=noop, .enable_cs_security=noop,
        .start_procedure=noop, .cleanup=noop
    };
    nrf_cs_step_t step = {.cycle_id=1, .attempt_id=1, .initiator=0,
        .reflector=1, .timeout_ms=500};
    assert(nrf_cs_pair_adapter_init(&s_adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_execute(&s_adapter, 0, &step, complete, NULL) == 0);
    conn->refs = 1;
    s_outgoing_conn = conn;
    s_outgoing_connected = false;
    s_completions = s_disconnects = 0;
    return step;
}

int main(void)
{
    struct bt_conn conn;
    s_radio_enabled=0;conn.refs=1;
    assert(atomic_get(&s_radio_enabled)==0);
    s_now_ms=500;connected_cb(&conn,0);
    assert(s_disconnects==1 && s_incoming_conn==NULL && conn.refs==1);
    assert(s_incoming_activity_seen==0);                /* a refused link is not activity of the duty */
    s_radio_enabled=1;
    s_incoming_disconnect_pending=1;
    connected_cb(&conn,0);
    assert(s_disconnects==2 && s_incoming_conn==NULL && conn.refs==1);
    s_incoming_disconnect_pending=0;
    nrf_cs_step_t step = start(&conn);
    assert(nrf_cs_pair_adapter_timeout(&s_adapter, &step) == 0);
    connected_cb(&conn, 2); /* Zephyr reports create cancellation here, not disconnected. */
    assert(s_outgoing_conn == NULL && conn.refs == 0);
    assert(s_completions == 1 && s_outcome == NRF_CS_OUTCOME_TIMEOUT);
    assert(!nrf_cs_pair_adapter_is_active(&s_adapter));
    connected_cb(&conn, 2);
    assert(s_completions == 1);

    step = start(&conn);
    assert(nrf_cs_pair_adapter_timeout(&s_adapter, &step) == 0);
    connected_cb(&conn, 0); /* Creation won the race with cancellation. */
    assert(s_disconnects == 1 && s_completions == 0);
    disconnected_cb(&conn, 0x13);
    assert(s_completions == 1 && s_outcome == NRF_CS_OUTCOME_TIMEOUT);
    assert(conn.refs == 0 && !nrf_cs_pair_adapter_is_active(&s_adapter));

    step = start(&conn);
    connected_cb(&conn, 0x3e); /* Ordinary create failure still enters transport cleanup. */
    assert(nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP);
    assert(s_outgoing_conn == NULL && conn.refs == 0);
    assert(nrf_cs_pair_adapter_cleanup_complete(&s_adapter,&step)==NRF_CS_PAIR_ADAPTER_COMPLETED);
    conn.refs=1;
    s_now_ms=1000;connected_cb(&conn,0); /* Radio-enabled idle reflector is still accepted. */
    assert(s_incoming_conn==&conn && conn.refs==2);
    assert(s_incoming_activity_seen==1 && s_incoming_activity_ms==1000);
    s_now_ms=1700;disconnected_cb(&conn,0x13);
    assert(s_incoming_conn==NULL && conn.refs==1 && s_incoming_activity_ms==1700);
    assert(s_incoming_disconnect_pending==0x14);
    s_incoming_disconnect_pending=0; /* This fixture does not run the event worker. */
    s_radio_enabled=0;
    const unsigned before=s_disconnects;
    connected_cb(&conn,0x3e);
    assert(s_disconnects==before && conn.refs==1 && s_incoming_conn==NULL);
    return 0;
}
