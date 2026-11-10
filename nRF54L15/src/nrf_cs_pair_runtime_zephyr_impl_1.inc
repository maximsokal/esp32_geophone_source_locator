LOG_MODULE_REGISTER(nrf_cs_pair_runtime, LOG_LEVEL_INF);

#define NRF_CS_CONFIG_ID 0U
#define NRF_CS_LOCAL_PROCEDURE_MEM \
    ((BT_RAS_MAX_STEPS_PER_PROCEDURE * sizeof(struct bt_le_cs_subevent_step)) + \
     (BT_RAS_MAX_STEPS_PER_PROCEDURE * BT_RAS_MAX_STEP_DATA_LEN))
#define NRF_CS_CHANNEL_INDEX_OFFSET 2U
#define NRF_CS_TONE_OK_THRESHOLD 15U
#define NRF_CS_EVENT_QUEUE_DEPTH 24U
#define NRF_CS_RANGING_COUNTER_NONE 0xFFFFU

#include "nrf_cs_pair_runtime_zephyr_workqueue.inc"

typedef enum {
    RUNTIME_EVENT_CONNECTED = 0,
    RUNTIME_EVENT_SECURITY_READY,
    RUNTIME_EVENT_MTU_READY,
    RUNTIME_EVENT_DISCOVERY_READY,
    RUNTIME_EVENT_FEATURES_READY,
    RUNTIME_EVENT_CAPABILITIES_READY,
    RUNTIME_EVENT_CONFIG_READY,
    RUNTIME_EVENT_CS_SECURITY_READY,
    RUNTIME_EVENT_TRANSPORT_ERROR,
    RUNTIME_EVENT_DISCONNECTED,
    RUNTIME_EVENT_LOCAL_COMPLETE,
    RUNTIME_EVENT_PEER_COMPLETE,
    RUNTIME_EVENT_RANGING_READY,
    RUNTIME_EVENT_PROCEDURE_ABORTED,
    RUNTIME_EVENT_INCOMING_CONNECTED,
    RUNTIME_EVENT_INCOMING_DISCONNECTED,
    RUNTIME_EVENT_REFLECTOR_CONFIG,
    RUNTIME_EVENT_RADIO_DISABLED,
} runtime_event_type_t;

typedef struct {
    runtime_event_type_t type;
    bool has_step;
    nrf_cs_step_t step;
    uint32_t value;
    int status;
    struct bt_conn *conn;
} runtime_event_t;

enum {
    RUNTIME_MEASUREMENT_OPEN,
    RUNTIME_MEASUREMENT_FAILED,
    RUNTIME_MEASUREMENT_COMMITTED,
};

/* The console trace (nrf_cs_trace.h) names every runtime event; the ESP32 log only shows how an attempt ended. */
static const char *runtime_event_name(runtime_event_type_t type)
{
    switch (type) {
        case RUNTIME_EVENT_CONNECTED: return "CONNECTED";
        case RUNTIME_EVENT_SECURITY_READY: return "SECURITY_READY";
        case RUNTIME_EVENT_MTU_READY: return "MTU_READY";
        case RUNTIME_EVENT_DISCOVERY_READY: return "DISCOVERY_READY";
        case RUNTIME_EVENT_FEATURES_READY: return "FEATURES_READY";
        case RUNTIME_EVENT_CAPABILITIES_READY: return "CAPABILITIES_READY";
        case RUNTIME_EVENT_CONFIG_READY: return "CONFIG_READY";
        case RUNTIME_EVENT_CS_SECURITY_READY: return "CS_SECURITY_READY";
        case RUNTIME_EVENT_TRANSPORT_ERROR: return "TRANSPORT_ERROR";
        case RUNTIME_EVENT_DISCONNECTED: return "DISCONNECTED";
        case RUNTIME_EVENT_LOCAL_COMPLETE: return "LOCAL_COMPLETE";
        case RUNTIME_EVENT_PEER_COMPLETE: return "PEER_COMPLETE";
        case RUNTIME_EVENT_RANGING_READY: return "RANGING_READY";
        case RUNTIME_EVENT_PROCEDURE_ABORTED: return "PROCEDURE_ABORTED";
        case RUNTIME_EVENT_INCOMING_CONNECTED: return "INCOMING_CONNECTED";
        case RUNTIME_EVENT_INCOMING_DISCONNECTED: return "INCOMING_DISCONNECTED";
        case RUNTIME_EVENT_REFLECTOR_CONFIG: return "REFLECTOR_CONFIG";
        case RUNTIME_EVENT_RADIO_DISABLED: return "RADIO_DISABLED";
        default: return "?";
    }
}

K_MSGQ_DEFINE(
    s_event_queue,
    sizeof(runtime_event_t),
    NRF_CS_EVENT_QUEUE_DEPTH,
    4);

static struct k_work s_event_work;
static struct k_work s_cleanup_work;
static struct k_work s_execute_work;
static struct k_work_delayable s_timeout_work;
static struct k_spinlock s_event_queue_overflow_lock;
static runtime_event_t s_overflow_error;
static bool s_overflow_error_pending;

static nrf_cs_pair_adapter_t s_adapter;
static bool s_started;
static atomic_t s_execute_pending;
static bool s_advertising;
/* Radio activity (advertising, scanning, connecting) is allowed while this is set. */
#if defined(CONFIG_APP_NRF_RADIO_ON_DEMAND) && CONFIG_APP_NRF_RADIO_ON_DEMAND
static atomic_t s_radio_enabled = ATOMIC_INIT(0);
#else
static atomic_t s_radio_enabled = ATOMIC_INIT(1);
#endif
static bool s_scanning;
static nrf_cs_node_id_t s_node_id = 0xFFU;
static nrf_cs_node_id_t s_scan_target = 0xFFU;

static struct bt_conn *s_outgoing_conn;
static bool s_outgoing_connected;
static atomic_t s_disconnect_retry_pending;
/* A rejected execute() asks the CS worker for one more try at a disconnect the controller kept refusing. */
static atomic_t s_disconnect_retry_request;
/* Zero means none; reason+1 keeps incoming disconnect ownership until worker delivery. */
static atomic_t s_incoming_disconnect_pending;
/* Last time an accepted incoming link came up or went down (uptime ms); the cycle runtime infers a release it never got
 * (nrf_cs_cycle_runtime_peer_silent) from how long this has been quiet. */
static atomic_t s_incoming_activity_ms;
static atomic_t s_incoming_activity_seen;
static void cleanup_work_handler(struct k_work *work);
static struct bt_conn *s_incoming_conn;

static nrf_cs_step_t s_timeout_step;
static struct k_spinlock s_cancel_lock;
static nrf_cs_step_t s_cancel_step;
static bool s_cancel_pending;
static int64_t s_attempt_deadline_ms;
static nrf_cs_rf_diagnostics_t s_rf_diagnostics;
static atomic_t s_rf_tx_power = ATOMIC_INIT(NRF_CS_RF_UNAVAILABLE);
static atomic_t s_rf_status;
static atomic_t s_rf_flags;
static atomic_t s_measurement_state = ATOMIC_INIT(RUNTIME_MEASUREMENT_OPEN);
static atomic_t s_procedure_accepted;
static nrf_cs_pair_runtime_complete_cb_t s_user_complete_cb;
static void *s_user_complete_context;

static uint8_t s_service_data[] = {
    BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL),
    0xFFU,
};

static const struct bt_data s_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL)),
    BT_DATA(BT_DATA_SVC_DATA16, s_service_data, sizeof(s_service_data)),
};

/*
 * How long a link may stay silent before the controller gives it up. The disconnect at the end of an attempt
 * is normally acknowledged at once; when the peer has already gone, the initiator waits for this timeout
 * before it can report the attempt and start the next one. With 4 s one lost link cost more than a whole
 * attempt budget (node log 2026-11-10 17:52: cleanup=3991ms, over_budget=1, timeout_in_cleanup=1). The link
 * runs at a 20 ms interval, so one second is fifty missed connection events: a link that silent is gone.
 */
#define NRF_CS_LINK_SUPERVISION_TIMEOUT_MS 1000

static const struct bt_le_conn_param s_conn_param = {
    .interval_min = 0x0010,
    .interval_max = 0x0010,
    .latency = 0,
    .timeout = BT_GAP_MS_TO_CONN_TIMEOUT(NRF_CS_LINK_SUPERVISION_TIMEOUT_MS),
};

/*
 * Finding the reflector is part of every attempt and comes out of its time budget. The reflector used to
 * advertise every 100-150 ms and the initiator listened for 30 ms out of every 60, so the connect took
 * anything from 138 ms to 1078 ms (same log), and the 1078 ms one ran the attempt out of its budget. The
 * reflector now advertises every 30-60 ms and the initiator listens without gaps. Both are only active for
 * the seconds of a ranging cycle (radio_on_demand), so the extra radio time does not cost battery at rest.
 */
#define NRF_CS_REFLECTOR_ADV_PARAM BT_LE_ADV_CONN_FAST_1
#define NRF_CS_INITIATOR_SCAN_PARAM BT_LE_SCAN_PASSIVE_CONTINUOUS

NET_BUF_SIMPLE_DEFINE_STATIC(s_local_steps, NRF_CS_LOCAL_PROCEDURE_MEM);
NET_BUF_SIMPLE_DEFINE_STATIC(s_peer_rx, BT_RAS_PROCEDURE_MEM);
NET_BUF_SIMPLE_DEFINE_STATIC(s_peer_owned, BT_RAS_PROCEDURE_MEM);

static uint16_t s_local_counter = NRF_CS_RANGING_COUNTER_NONE;
static uint16_t s_peer_counter = NRF_CS_RANGING_COUNTER_NONE;
static uint16_t s_ready_counter = NRF_CS_RANGING_COUNTER_NONE;
static bool s_local_complete;
static bool s_peer_complete;
/* Completion events queue counters, while IQ buffers must survive until the worker consumes them. */
static atomic_t s_local_data_frozen;
static atomic_t s_peer_data_frozen;
static atomic_t s_expected_counter = ATOMIC_INIT(NRF_CS_RANGING_COUNTER_NONE);
static bool s_realtime_ras;
static bool s_on_demand_request_started;
static atomic_t s_reflector_procedure_active;
static uint32_t s_event_queue_overflow_total;
static struct bt_conn_le_cs_config s_cs_config;

static uint16_t s_iq_counts[CS_DE_NUM_CHANNELS];
static cs_de_report_t s_de_report;
/* Owned by the serialized CS work item; cs_de_ifft overwrites its input. */
static float s_iq_ifft_scratch[2U * CONFIG_BT_CS_DE_NFFT_SIZE];

static void runtime_post_event(
    runtime_event_type_t type,
    const nrf_cs_step_t *step,
    uint32_t value,
    int status,
    struct bt_conn *conn)
{
    runtime_event_t event = {
        .type = type,
        .has_step = step != NULL,
        .value = value,
        .status = status,
        .conn = conn != NULL ? bt_conn_ref(conn) : NULL,
    };
    const bool invalidates_result = type == RUNTIME_EVENT_TRANSPORT_ERROR ||
        type == RUNTIME_EVENT_DISCONNECTED || type == RUNTIME_EVENT_PROCEDURE_ABORTED;
    const bool recover_terminal = type == RUNTIME_EVENT_TRANSPORT_ERROR ||
        (invalidates_result && nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);

    if (step != NULL) {
        event.step = *step;
    }

    if (invalidates_result && event.has_step &&
        nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_PROCEDURE &&
        nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, &event.step)) {
        (void)atomic_cas(&s_measurement_state, RUNTIME_MEASUREMENT_OPEN, RUNTIME_MEASUREMENT_FAILED);
    }

    if (k_msgq_put(&s_event_queue, &event, K_NO_WAIT) != 0) {
        uint32_t overflow_total;
        k_spinlock_key_t key;

        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
        key = k_spin_lock(&s_event_queue_overflow_lock);
        nrf_cs_event_overflow_increment(&s_event_queue_overflow_total);
        overflow_total = s_event_queue_overflow_total;
        if (recover_terminal && event.has_step &&
            nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, &event.step) &&
            (!s_overflow_error_pending || !nrf_cs_pair_runtime_bridge_steps_equal(&s_overflow_error.step, &event.step))) {
            s_overflow_error = event;
            s_overflow_error.conn = NULL;
            s_overflow_error_pending = true;
        }
        k_spin_unlock(&s_event_queue_overflow_lock, key);
        nrf_log_runtime_zephyr_append_event(
            NRF_LOG_EVENT_CS_EVENT_QUEUE_OVERFLOW,
            overflow_total,
            NRF_CS_EVENT_QUEUE_DEPTH);
        LOG_ERR("Channel Sounding event queue overflow");
        if (type == RUNTIME_EVENT_INCOMING_DISCONNECTED || recover_terminal) {
            (void)submit_cs_work(&s_event_work);
        }
        return;
    }
    (void)submit_cs_work(&s_event_work);
}

static bool runtime_take_overflow_error(runtime_event_t *event)
{
    k_spinlock_key_t key = k_spin_lock(&s_event_queue_overflow_lock);
    const bool pending = s_overflow_error_pending;
    if (pending) {
        *event = s_overflow_error;
        s_overflow_error_pending = false;
    }
    k_spin_unlock(&s_event_queue_overflow_lock, key);
    return pending;
}

static bool runtime_active_step(nrf_cs_step_t *step_out)
{
    const nrf_cs_step_t *active = nrf_cs_pair_adapter_active_step(&s_adapter);
    if (active == NULL || step_out == NULL) {
        return false;
    }
    *step_out = *active;
    return true;
}

static bool runtime_active_step_for_conn(struct bt_conn *conn, nrf_cs_step_t *step_out)
{
    if (conn == NULL || conn != s_outgoing_conn) {
        return false;
    }
    return runtime_active_step(step_out);
}

static bool runtime_step_matches_active(const nrf_cs_step_t *step)
{
    return nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, step);
}

static bool runtime_active_procedure_for_conn(struct bt_conn *conn, nrf_cs_step_t *step_out)
{
    return nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_PROCEDURE &&
        runtime_active_step_for_conn(conn, step_out);
}

void nrf_cs_pair_runtime_zephyr_cancel(const nrf_cs_step_t *step)
{
    if (!s_started || step == NULL) return;
    if (nrf_cs_pair_adapter_is_active(&s_adapter) &&
        !nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, step)) return;
    if (atomic_get(&s_execute_pending) != 0 &&
        !nrf_cs_pair_runtime_bridge_steps_equal(&s_timeout_step, step)) return;
    k_spinlock_key_t key = k_spin_lock(&s_cancel_lock);
    s_cancel_step = *step;
    s_cancel_pending = true;
    /* The worker may be inside DSP; block publish before it observes this mailbox. */
    if (nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_PROCEDURE &&
        nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, step)) {
        (void)atomic_cas(&s_measurement_state, RUNTIME_MEASUREMENT_OPEN, RUNTIME_MEASUREMENT_FAILED);
    }
    k_spin_unlock(&s_cancel_lock, key);
    (void)submit_cs_work(&s_event_work);
}

static bool runtime_take_cancel(const nrf_cs_step_t *step)
{
    k_spinlock_key_t key = k_spin_lock(&s_cancel_lock);
    const bool cancelled = s_cancel_pending &&
        nrf_cs_pair_runtime_bridge_steps_equal(&s_cancel_step, step);
    if (cancelled) s_cancel_pending = false;
    k_spin_unlock(&s_cancel_lock, key);
    return cancelled;
}

static int runtime_start_advertising(void)
{
    int err;

    if (atomic_get(&s_radio_enabled) == 0 || !s_started || s_node_id == 0xFFU || s_advertising ||
        s_scanning || s_outgoing_conn != NULL || s_incoming_conn != NULL ||
        nrf_cs_pair_adapter_is_active(&s_adapter)) {
        return 0;
    }

    s_service_data[2] = s_node_id;
    err = bt_le_adv_start(NRF_CS_REFLECTOR_ADV_PARAM, s_ad, ARRAY_SIZE(s_ad), NULL, 0);
    if (err == 0 || err == -EALREADY) {
        s_advertising = true;
        return 0;
    }

    LOG_ERR("RAS advertising start failed: %d", err);
    return err;
}

static int runtime_stop_advertising(void)
{
    int err;

    if (!s_advertising) {
        return 0;
    }

    err = bt_le_adv_stop();
    if (err != 0 && err != -EALREADY) {
        LOG_WRN("RAS advertising stop failed: %d", err);
        return err;
    }
    s_advertising = false;
    return 0;
}

static void runtime_stop_scan(void)
{
    int err;

    if (!s_scanning) {
        return;
    }

    err = bt_le_scan_stop();
    if (err != 0 && err != -EALREADY) {
        LOG_WRN("CS scan stop failed: %d", err);
    }
    s_scanning = false;
}

/* Expected counter is in the 12-bit RAS domain; NONE keeps the cold-connection path. */
static bool runtime_reset_procedure(uint16_t expected_counter)
{
    /* A warm reset belongs to the CS worker, after both callback buffers are immutable. */
    if (expected_counter != NRF_CS_RANGING_COUNTER_NONE &&
        (expected_counter > 0xFFFU || nrf_cs_pair_adapter_is_active(&s_adapter) ||
         !s_local_complete || !s_peer_complete ||
         atomic_get(&s_local_data_frozen) == 0 || atomic_get(&s_peer_data_frozen) == 0 ||
         s_local_counter == NRF_CS_RANGING_COUNTER_NONE || s_local_counter != s_peer_counter ||
         expected_counter == s_local_counter)) {
        return false;
    }
    atomic_set(&s_expected_counter, expected_counter);
    atomic_set(&s_measurement_state, RUNTIME_MEASUREMENT_OPEN);
    atomic_set(&s_procedure_accepted, 0);
    nrf_cs_rf_reset(&s_rf_diagnostics);
    atomic_set(&s_rf_tx_power, NRF_CS_RF_UNAVAILABLE);
    atomic_set(&s_rf_status, (atomic_val_t)nrf_cs_rf_status_word(127, 127, 0, 0, 0, false));
    atomic_set(&s_rf_flags, 0);
    net_buf_simple_reset(&s_local_steps);
    net_buf_simple_reset(&s_peer_owned);
    s_local_counter = NRF_CS_RANGING_COUNTER_NONE;
    s_peer_counter = NRF_CS_RANGING_COUNTER_NONE;
    s_ready_counter = NRF_CS_RANGING_COUNTER_NONE;
    s_local_complete = false;
    s_peer_complete = false;
    atomic_set(&s_local_data_frozen, 0);
    atomic_set(&s_peer_data_frozen, 0);
    s_on_demand_request_started = false;
    return true;
}

static void runtime_reset_ranging(void)
{
    (void)runtime_reset_procedure(NRF_CS_RANGING_COUNTER_NONE);
    /* RAS owns segmented RX between callbacks; only full teardown may discard it here. */
    net_buf_simple_reset(&s_peer_rx);
    atomic_set(&s_disconnect_retry_pending, 0);
    atomic_set(&s_disconnect_retry_request, 0);
    s_realtime_ras = false;
    atomic_set(&s_reflector_procedure_active, 0);
    memset(&s_cs_config, 0, sizeof(s_cs_config));
}

static bool runtime_counter_matches(uint16_t counter)
{
    const atomic_val_t expected = atomic_get(&s_expected_counter);
    return expected == NRF_CS_RANGING_COUNTER_NONE || counter == (uint16_t)expected;
}

typedef struct {
    nrf_cs_node_id_t wanted_node;
    bool matched;
} scan_match_t;

static bool scan_ad_parse(struct bt_data *data, void *user_data)
{
    scan_match_t *match = user_data;

    if (data == NULL || match == NULL) {
        return true;
    }
    if (data->type != BT_DATA_SVC_DATA16 || data->data_len < 3U) {
        return true;
    }
    if (sys_get_le16(data->data) != BT_UUID_RANGING_SERVICE_VAL) {
        return true;
    }
    if (data->data[2] != match->wanted_node) {
        return true;
    }

    match->matched = true;
    return false;
}

static void scan_device_found(
    const bt_addr_le_t *addr,
    int8_t rssi,
    uint8_t type,
    struct net_buf_simple *ad)
{
    scan_match_t match = {
        .wanted_node = s_scan_target,
        .matched = false,
    };
    nrf_cs_step_t step;
    int err;

    ARG_UNUSED(rssi);

    if (!runtime_active_step(&step) ||
        nrf_cs_pair_adapter_phase(&s_adapter) != NRF_CS_PAIR_PHASE_CONNECTING ||
        s_outgoing_conn != NULL) {
        return;
    }
    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
        type != BT_GAP_ADV_TYPE_EXT_ADV) {
        return;
    }

    bt_data_parse(ad, scan_ad_parse, &match);
    if (!match.matched) {
        return;
    }

    runtime_stop_scan();
    err = bt_conn_le_create(
        addr,
        BT_CONN_LE_CREATE_CONN,
        &s_conn_param,
        &s_outgoing_conn);
    if (err != 0) {
        s_outgoing_conn = NULL;
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, err, NULL);
    }
}

static int op_connect(void *context, nrf_cs_node_id_t reflector)
{
    int err;

    ARG_UNUSED(context);
    if (atomic_get(&s_radio_enabled) == 0) {
        return -ENOTSUP;
    }
    if (s_outgoing_conn != NULL || s_incoming_conn != NULL || s_scanning) {
        return -EBUSY;
    }

    err = runtime_stop_advertising();
    if (err != 0) {
        return err;
    }
    runtime_reset_ranging();
    s_scan_target = reflector;

    err = bt_le_scan_start(NRF_CS_INITIATOR_SCAN_PARAM, scan_device_found);
    if (err != 0) {
        return err;
    }
    s_scanning = true;
    return 0;
}

static int op_secure(void *context)
{
    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL || !s_outgoing_connected) {
        return -ENOTCONN;
    }
    return bt_conn_set_security(s_outgoing_conn, BT_SECURITY_L2);
}

static void mtu_exchange_cb(
    struct bt_conn *conn,
    uint8_t err,
    struct bt_gatt_exchange_params *params)
{
    nrf_cs_step_t step;

    ARG_UNUSED(params);
    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    runtime_post_event(
        err == 0U ? RUNTIME_EVENT_MTU_READY : RUNTIME_EVENT_TRANSPORT_ERROR,
        &step,
        0U,
        (int)err,
        NULL);
}

static struct bt_gatt_exchange_params s_mtu_params = {
    .func = mtu_exchange_cb,
};

static int op_exchange_mtu(void *context)
{
    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    return bt_gatt_exchange_mtu(s_outgoing_conn, &s_mtu_params);
}

static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
    struct bt_conn *conn;
    nrf_cs_step_t step;
    int err;
    int release_err;

    ARG_UNUSED(context);
    conn = bt_gatt_dm_conn_get(dm);
    if (!runtime_active_step_for_conn(conn, &step)) {
        (void)bt_gatt_dm_data_release(dm);
        return;
    }

    err = bt_ras_rreq_alloc_and_assign_handles(dm, conn);
    release_err = bt_gatt_dm_data_release(dm);
    if (err != 0 || release_err != 0) {
        runtime_post_event(
            RUNTIME_EVENT_TRANSPORT_ERROR,
            &step,
            0U,
            err != 0 ? err : release_err,
            NULL);
        return;
    }

    runtime_post_event(RUNTIME_EVENT_DISCOVERY_READY, &step, 0U, 0, NULL);
}

static void discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
    nrf_cs_step_t step;

    ARG_UNUSED(context);
    if (runtime_active_step_for_conn(conn, &step)) {
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, -ENOENT, NULL);
    }
}

static void discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
    nrf_cs_step_t step;

    ARG_UNUSED(context);
    if (runtime_active_step_for_conn(conn, &step)) {
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, err, NULL);
    }
}

static struct bt_gatt_dm_cb s_discovery_cb = {
    .completed = discovery_completed_cb,
    .service_not_found = discovery_service_not_found_cb,
    .error_found = discovery_error_found_cb,
};

static int op_discover_ras(void *context)
{
    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    return bt_gatt_dm_start(
        s_outgoing_conn,
        BT_UUID_RANGING_SERVICE,
        &s_discovery_cb,
        NULL);
}

static void ras_features_read_cb(struct bt_conn *conn, uint32_t feature_bits, int err)
{
    nrf_cs_step_t step;

    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    runtime_post_event(
        err == 0 ? RUNTIME_EVENT_FEATURES_READY : RUNTIME_EVENT_TRANSPORT_ERROR,
        &step,
        feature_bits,
        err,
        NULL);
}

static int op_read_features(void *context)
{
    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    return bt_ras_rreq_read_features(s_outgoing_conn, ras_features_read_cb);
}

static void ranging_data_received_cb(
    struct bt_conn *conn,
    uint16_t ranging_counter,
    int err)
{
    nrf_cs_step_t step;
    size_t len;

    if (!runtime_active_procedure_for_conn(conn, &step)) {
        return;
    }
    if (!runtime_counter_matches(ranging_counter)) {
        return;
    }
    if (atomic_get(&s_peer_data_frozen) != 0) {
        return;
    }
    if (err != 0) {
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, err, NULL);
        return;
    }

    net_buf_simple_reset(&s_peer_owned);
    len = s_peer_rx.len;
    if (len == 0U || len > net_buf_simple_tailroom(&s_peer_owned)) {
        runtime_post_event(RUNTIME_EVENT_PROCEDURE_ABORTED, &step, 0U, -ENOBUFS, NULL);
        return;
    }
    net_buf_simple_add_mem(&s_peer_owned, s_peer_rx.data, len);
    atomic_set(&s_peer_data_frozen, 1);
    runtime_post_event(RUNTIME_EVENT_PEER_COMPLETE, &step, ranging_counter, 0, NULL);
}

static void ranging_data_ready_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
    nrf_cs_step_t step;

    if (!runtime_counter_matches(ranging_counter)) {
        return;
    }
    if (runtime_active_procedure_for_conn(conn, &step)) {
        runtime_post_event(RUNTIME_EVENT_RANGING_READY, &step, ranging_counter, 0, NULL);
    }
}

static void ranging_data_overwritten_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
    nrf_cs_step_t step;

    if (runtime_active_procedure_for_conn(conn, &step) &&
        s_ready_counter == ranging_counter &&
        atomic_get(&s_peer_data_frozen) == 0 &&
        !s_peer_complete) {
        runtime_post_event(RUNTIME_EVENT_PROCEDURE_ABORTED, &step, ranging_counter, -ENODATA, NULL);
    }
}
