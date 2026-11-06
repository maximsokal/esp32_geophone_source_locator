static void configure_reflector_connection(struct bt_conn *conn)
{
    int err;

    if (conn == NULL) {
        return;
    }
    err = set_default_cs_settings(conn, false);
    if (err != 0) {
        LOG_WRN("Reflector default CS settings failed: %d", err);
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void configure_reflector_procedure(struct bt_conn *conn, uint8_t config_id)
{
    struct bt_le_cs_set_procedure_parameters_param params;
    int err;

    if (conn == NULL) {
        return;
    }
    fill_procedure_params(&params, config_id, true);
    err = bt_le_cs_set_procedure_parameters(conn, &params);
    if (err != 0) {
        LOG_WRN("Reflector procedure parameters failed: %d", err);
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* Synchronous HCI calls here can delay the CS timeout, so this handler uses the dedicated queue. */
static void event_work_handler(struct k_work *work)
{
    runtime_event_t event;

    ARG_UNUSED(work);
    /* One try per rejected execute(); op_cleanup() re-arms the pending flag if the controller refuses again. */
    if (atomic_cas(&s_disconnect_retry_request, 1, 0) &&
        nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP &&
        s_outgoing_conn != NULL && atomic_cas(&s_disconnect_retry_pending, 1, 0)) {
        (void)op_cleanup(NULL);
    }
    for (;;) {
        nrf_cs_step_t cancelled_step;
        if (runtime_active_step(&cancelled_step) && runtime_take_cancel(&cancelled_step)) {
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED,
                cancelled_step.attempt_id, (uint32_t)-ECANCELED);
            (void)nrf_cs_pair_adapter_transport_error(&s_adapter, &cancelled_step);
        }
        const bool has_event = runtime_take_overflow_error(&event) || k_msgq_get(&s_event_queue, &event, K_NO_WAIT) == 0;
        if (!has_event) break;
        if (event.has_step && !runtime_step_matches_active(&event.step)) {
            if (event.conn != NULL) {
                bt_conn_unref(event.conn);
            }
            continue;
        }
        /* Which event arrived in which phase, and when: the stage times the time budget of one pair is built from. */
        nrf_cs_trace_zephyr_event(runtime_event_name(event.type), event.has_step ? event.step.attempt_id : 0U,
            event.status, (uint8_t)nrf_cs_pair_adapter_phase(&s_adapter));
        if (event.has_step && event.status != 0) {
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_PAIR_PHASE,
                event.step.attempt_id, (uint32_t)nrf_cs_pair_adapter_phase(&s_adapter));
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED,
                event.step.attempt_id, (uint32_t)event.status);
        }
        if (event.has_step && nrf_cs_pair_adapter_phase(&s_adapter) != NRF_CS_PAIR_PHASE_CLEANUP &&
            k_uptime_get() >= s_attempt_deadline_ms) {
            /* A busy HCI worker may receive data before its due timeout work runs. */
            timeout_work_handler(NULL);
        }
        if (event.has_step && event.type != RUNTIME_EVENT_DISCONNECTED &&
            nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
            if (event.conn != NULL) bt_conn_unref(event.conn);
            continue;
        }
        if (event.conn != NULL &&
            (event.type == RUNTIME_EVENT_INCOMING_CONNECTED ||
             event.type == RUNTIME_EVENT_REFLECTOR_CONFIG ||
             event.type == RUNTIME_EVENT_RADIO_DISABLED) &&
            (event.conn != s_incoming_conn || atomic_get(&s_radio_enabled) == 0)) {
            if (event.conn == s_incoming_conn) {
                (void)bt_conn_disconnect(event.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            }
            bt_conn_unref(event.conn);
            continue;
        }
        switch (event.type) {
        case RUNTIME_EVENT_CONNECTED:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_CONNECTED, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_SECURITY_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_SECURITY_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_MTU_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_MTU_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_DISCOVERY_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_DISCOVERY_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_FEATURES_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_FEATURES_READY, &event.step,
                (event.value & RAS_FEAT_REALTIME_RD) != 0U, NULL);
            break;
        case RUNTIME_EVENT_CAPABILITIES_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_CAPABILITIES_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_CONFIG_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_CONFIG_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_CS_SECURITY_READY:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_CS_SECURITY_READY, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_TRANSPORT_ERROR:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_TRANSPORT_ERROR, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_DISCONNECTED:
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_DISCONNECTED, &event.step, false, NULL);
            break;
        case RUNTIME_EVENT_LOCAL_COMPLETE:
            s_local_counter = (uint16_t)event.value;
            (void)nrf_cs_pair_runtime_bridge_record_completion(
                &s_local_complete, &s_peer_complete, true);
            maybe_request_on_demand_data();
            maybe_complete_measurement(&event.step);
            break;
        case RUNTIME_EVENT_PEER_COMPLETE:
            s_peer_counter = (uint16_t)event.value;
            (void)nrf_cs_pair_runtime_bridge_record_completion(
                &s_local_complete, &s_peer_complete, false);
            maybe_complete_measurement(&event.step);
            break;
        case RUNTIME_EVENT_RANGING_READY:
            s_ready_counter = (uint16_t)event.value;
            maybe_request_on_demand_data();
            break;
        case RUNTIME_EVENT_PROCEDURE_ABORTED: {
            nrf_cs_measurement_t measurement = {
                .outcome = NRF_CS_OUTCOME_OUTLIER,
                .distance_mm = 0,
                .confidence = 0U,
                .provider_quality_flags =
                    s_realtime_ras ? NRF_CS_PROVIDER_QUALITY_REALTIME_RAS : 0U,
            };
            (void)nrf_cs_pair_runtime_bridge_dispatch(
                &s_adapter, NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY,
                &event.step, false, &measurement);
            break;
        }
        case RUNTIME_EVENT_INCOMING_CONNECTED:
            configure_reflector_connection(event.conn);
            break;
        case RUNTIME_EVENT_INCOMING_DISCONNECTED:
            (void)runtime_start_advertising();
            if (event.value == NRF_CS_CYCLE_RELEASE_REASON && s_relay_released_cb != NULL) {
                s_relay_released_cb();
            }
            atomic_set(&s_incoming_disconnect_pending, 0);
            break;
        case RUNTIME_EVENT_REFLECTOR_CONFIG:
            configure_reflector_procedure(event.conn, (uint8_t)event.value);
            break;
        case RUNTIME_EVENT_RADIO_DISABLED:
            break; /* Radio was enabled again before the queued stop was consumed. */
        default:
            break;
        }

        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
        if (event.type != RUNTIME_EVENT_DISCONNECTED &&
            event.type != RUNTIME_EVENT_INCOMING_DISCONNECTED) {
            nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);
        }
    }
    nrf_cs_step_t ready_step;
    if (runtime_active_procedure_for_conn(s_outgoing_conn, &ready_step)) {
        if (k_uptime_get() >= s_attempt_deadline_ms) timeout_work_handler(NULL);
        else maybe_complete_measurement(&ready_step);
    }
    const atomic_val_t incoming_disconnect = atomic_get(&s_incoming_disconnect_pending);
    if (incoming_disconnect != 0 && s_incoming_conn == NULL) {
        nrf_cs_trace_zephyr_event("INCOMING_DISCONNECTED", 0U, (int)incoming_disconnect - 1,
            (uint8_t)nrf_cs_pair_adapter_phase(&s_adapter));
        if (incoming_disconnect - 1 == NRF_CS_CYCLE_RELEASE_REASON && s_relay_released_cb != NULL) {
            s_relay_released_cb();
        }
        atomic_set(&s_incoming_disconnect_pending, 0);
        (void)runtime_start_advertising();
    }
    /* DISCONNECTED may have overflowed after the deadline had already run. */
    if (s_outgoing_conn == NULL &&
        nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
        cleanup_work_handler(NULL);
    }
}

#include "nrf_cs_pair_runtime_zephyr_connections.inc"

static void security_changed_cb(
    struct bt_conn *conn,
    bt_security_t level,
    enum bt_security_err err)
{
    nrf_cs_step_t step;

    ARG_UNUSED(level);
    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    runtime_post_event(
        err == 0 ? RUNTIME_EVENT_SECURITY_READY : RUNTIME_EVENT_TRANSPORT_ERROR,
        &step,
        0U,
        (int)err,
        NULL);
}

static void remote_capabilities_cb(
    struct bt_conn *conn,
    uint8_t status,
    struct bt_conn_le_cs_capabilities *params)
{
    nrf_cs_step_t step;

    ARG_UNUSED(params);
    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    runtime_post_event(
        status == BT_HCI_ERR_SUCCESS ?
            RUNTIME_EVENT_CAPABILITIES_READY :
            RUNTIME_EVENT_TRANSPORT_ERROR,
        &step,
        0U,
        (int)status,
        NULL);
}

static void config_complete_cb(
    struct bt_conn *conn,
    uint8_t status,
    struct bt_conn_le_cs_config *config)
{
    nrf_cs_step_t step;

    if (conn == s_outgoing_conn && runtime_active_step(&step)) {
        if (status == BT_HCI_ERR_SUCCESS && config != NULL) {
            s_cs_config = *config;
            runtime_post_event(RUNTIME_EVENT_CONFIG_READY, &step, 0U, 0, NULL);
        } else {
            runtime_post_event(
                RUNTIME_EVENT_TRANSPORT_ERROR,
                &step,
                0U,
                (int)status,
                NULL);
        }
        return;
    }

    if (conn == s_incoming_conn &&
        status == BT_HCI_ERR_SUCCESS &&
        config != NULL &&
        config->role == BT_CONN_LE_CS_ROLE_REFLECTOR) {
        runtime_post_event(
            RUNTIME_EVENT_REFLECTOR_CONFIG,
            NULL,
            config->id,
            0,
            conn);
    }
}

static void cs_security_complete_cb(struct bt_conn *conn, uint8_t status)
{
    nrf_cs_step_t step;

    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    runtime_post_event(
        status == BT_HCI_ERR_SUCCESS ?
            RUNTIME_EVENT_CS_SECURITY_READY :
            RUNTIME_EVENT_TRANSPORT_ERROR,
        &step,
        0U,
        (int)status,
        NULL);
}

static void procedure_enable_complete_cb(
    struct bt_conn *conn,
    uint8_t status,
    struct bt_conn_le_cs_procedure_enable_complete *params)
{
    nrf_cs_step_t step;
    const bool enabled = status == BT_HCI_ERR_SUCCESS && params != NULL &&
        params->state == BT_CONN_LE_CS_PROCEDURES_ENABLED;

    if (conn == NULL || (conn != s_incoming_conn && !runtime_active_procedure_for_conn(conn, &step))) {
        return;
    }
    if (conn == s_outgoing_conn && enabled && params->config_id != NRF_CS_CONFIG_ID) {
        return;
    }
    if ((conn == s_incoming_conn || conn == s_outgoing_conn) && enabled) {
        atomic_set(&s_rf_tx_power, params->selected_tx_power);
        if (params->tone_antenna_config_selection != BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1) {
            atomic_or(&s_rf_flags, NRF_CS_RF_ANTENNA_MISMATCH);
        }
    }

    if (conn == s_incoming_conn) {
        if (status == BT_HCI_ERR_SUCCESS && params != NULL) {
            atomic_set(&s_reflector_procedure_active, enabled ? 1 : 0);
            nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);
        }
        return;
    }
    if (status != BT_HCI_ERR_SUCCESS) {
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, (int)status, NULL);
        return;
    }

    if (enabled) {
        atomic_set(&s_procedure_accepted, 1);
        (void)submit_cs_work(&s_event_work);
    }
}

static void subevent_data_cb(
    struct bt_conn *conn,
    struct bt_conn_le_cs_subevent_result *result)
{
    nrf_cs_step_t step;
    uint16_t ranging_counter;
    size_t len;

    if (result == NULL || conn == NULL) {
        return;
    }
    if (conn != s_incoming_conn && !runtime_active_procedure_for_conn(conn, &step)) {
        return;
    }
    if (conn == s_outgoing_conn && result->header.config_id != NRF_CS_CONFIG_ID) {
        return;
    }
    if (conn == s_outgoing_conn && atomic_get(&s_local_data_frozen) != 0) {
        return;
    }
    if (conn == s_outgoing_conn && !runtime_counter_matches(
            bt_ras_rreq_get_ranging_counter(result->header.procedure_counter))) {
        return;
    }
    if (conn == s_incoming_conn || conn == s_outgoing_conn) {
        const bool aborted = result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED ||
            result->header.subevent_done_status == BT_CONN_LE_CS_SUBEVENT_ABORTED;
        const uint32_t rf_status = nrf_cs_rf_status_word(result->header.reference_power_level,
            (int8_t)atomic_get(&s_rf_tx_power), result->header.num_antenna_paths,
            result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED ?
                (uint8_t)result->header.procedure_abort_reason : 0U,
            result->header.subevent_done_status == BT_CONN_LE_CS_SUBEVENT_ABORTED ?
                (uint8_t)result->header.subevent_abort_reason : 0U, conn == s_incoming_conn);
        atomic_set(&s_rf_status, (atomic_val_t)rf_status);
        atomic_or(&s_rf_flags, NRF_CS_RF_METADATA | (aborted ? NRF_CS_RF_ABORTED : 0U) |
            (result->header.num_antenna_paths > 1U ? NRF_CS_RF_ANTENNA_MISMATCH : 0U));
        if (conn == s_incoming_conn && (aborted || result->header.num_antenna_paths > 1U)) {
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_RF_STATUS,
                result->header.procedure_counter, rf_status);
        }
    }
    if (conn == s_incoming_conn) {
        nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);
        if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE ||
            result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED) {
            atomic_set(&s_reflector_procedure_active, 0);
        }
        return;
    }
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);

    ranging_counter =
        bt_ras_rreq_get_ranging_counter(result->header.procedure_counter);

    if (s_local_counter != NRF_CS_RANGING_COUNTER_NONE &&
        s_local_counter != ranging_counter &&
        !s_local_complete) {
        net_buf_simple_reset(&s_local_steps);
    }
    s_local_counter = ranging_counter;

    if (result->header.subevent_done_status != BT_CONN_LE_CS_SUBEVENT_ABORTED &&
        result->step_data_buf != NULL) {
        len = result->step_data_buf->len;
        if (len > net_buf_simple_tailroom(&s_local_steps)) {
            atomic_set(&s_local_data_frozen, 1);
            runtime_post_event(
                RUNTIME_EVENT_PROCEDURE_ABORTED,
                &step,
                ranging_counter,
                -ENOBUFS,
                NULL);
            return;
        }
        net_buf_simple_add_mem(&s_local_steps, result->step_data_buf->data, len);
    }

    if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED) {
        atomic_set(&s_local_data_frozen, 1);
        runtime_post_event(
            RUNTIME_EVENT_PROCEDURE_ABORTED,
            &step,
            ranging_counter,
            -ECANCELED,
            NULL);
    } else if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
        atomic_set(&s_local_data_frozen, 1);
        runtime_post_event(
            RUNTIME_EVENT_LOCAL_COMPLETE,
            &step,
            ranging_counter,
            0,
            NULL);
    }
}

BT_CONN_CB_DEFINE(s_conn_callbacks) = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
    .security_changed = security_changed_cb,
    .le_cs_read_remote_capabilities_complete = remote_capabilities_cb,
    .le_cs_config_complete = config_complete_cb,
    .le_cs_security_enable_complete = cs_security_complete_cb,
    .le_cs_procedure_enable_complete = procedure_enable_complete_cb,
    .le_cs_subevent_data_available = subevent_data_cb,
};

static void execute_work_handler(struct k_work *work)
{
    nrf_cs_pair_adapter_result_t result;
    nrf_cs_measurement_t failure = {
        .outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR,
    };

    ARG_UNUSED(work);
    if (runtime_take_cancel(&s_timeout_step)) {
        atomic_set(&s_execute_pending, 0);
        runtime_adapter_complete(&s_timeout_step, &failure, NULL);
        return;
    }
    const int64_t remaining_ms = s_attempt_deadline_ms - k_uptime_get();
    if (remaining_ms <= 0) {
        failure.outcome = NRF_CS_OUTCOME_TIMEOUT;
        atomic_set(&s_execute_pending, 0);
        runtime_adapter_complete(&s_timeout_step, &failure, NULL);
        return;
    }
    /* The elapsed time of this line is how long the attempt waited for the work queue. */
    nrf_cs_trace_zephyr_event("EXECUTE_WORK_STARTED", s_timeout_step.attempt_id, 0,
        (uint8_t)nrf_cs_pair_adapter_phase(&s_adapter));
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_PAIR_CONTEXT,
        s_timeout_step.cycle_id, ((uint32_t)s_timeout_step.initiator << 24U) |
        ((uint32_t)s_timeout_step.reflector << 16U) |
        ((uint32_t)s_timeout_step.attempt_number << 8U) | s_timeout_step.pair_index);
    /* Queue wait and synchronous BLE setup consume the same accepted attempt budget. */
    if (reschedule_cs_work(&s_timeout_work, K_MSEC((uint32_t)remaining_ms)) < 0) {
        atomic_set(&s_execute_pending, 0);
        runtime_adapter_complete(&s_timeout_step, &failure, NULL);
        return;
    }
    result = nrf_cs_pair_adapter_execute(
        &s_adapter,
        s_node_id,
        &s_timeout_step,
        runtime_adapter_complete,
        &s_adapter);
    atomic_set(&s_execute_pending, 0);
    if (result != NRF_CS_PAIR_ADAPTER_OK) {
        runtime_adapter_complete(&s_timeout_step, &failure, NULL);
        return;
    }

    if (k_uptime_get() >= s_attempt_deadline_ms) timeout_work_handler(NULL);
}

int nrf_cs_pair_runtime_zephyr_start(void)
{
    int err;

    if (s_started) {
        return -EALREADY;
    }

    k_msgq_purge(&s_event_queue);
    k_work_init(&s_event_work, event_work_handler);
    k_work_init(&s_cleanup_work, cleanup_work_handler);
    k_work_init(&s_execute_work, execute_work_handler);
    k_work_init_delayable(&s_timeout_work, timeout_work_handler);
    start_cs_workqueue();
    runtime_reset_ranging();
    atomic_set(&s_execute_pending, 0);
    s_event_queue_overflow_total = 0U;

    if (nrf_cs_pair_adapter_init(&s_adapter, &s_adapter_ops) !=
        NRF_CS_PAIR_ADAPTER_OK) {
        return -EINVAL;
    }

    err = bt_enable(NULL);
    if (err != 0) {
        return err;
    }

    s_started = true;
    return 0;
}

int nrf_cs_pair_runtime_zephyr_set_node_id(nrf_cs_node_id_t node_id)
{
    if (node_id == 0xFFU) {
        return -EINVAL;
    }
    if (!s_started) {
        /* Optional Bluetooth CS startup failed: sensors/TWIS may still run. */
        return -EAGAIN;
    }
    if (s_node_id != 0xFFU && s_node_id != node_id) {
        return -EALREADY;
    }

    s_node_id = node_id;
    s_service_data[2] = node_id;
    return runtime_start_advertising();
}

int nrf_cs_pair_runtime_zephyr_set_radio_enabled(bool enabled)
{
    if (!s_started) {
        return -EINVAL;
    }
    atomic_set(&s_radio_enabled, enabled ? 1 : 0);
    if (enabled) {
        return runtime_start_advertising();
    }
    /* Outgoing attempts retain their timeout; close an owned reflector on the CS queue. */
    runtime_stop_advertising();
    if (s_incoming_conn != NULL) {
        runtime_post_event(RUNTIME_EVENT_RADIO_DISABLED, NULL, 0U, 0, s_incoming_conn);
    }
    return 0;
}

int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context)
{
    int submit_result;

    if (!s_started || step == NULL || complete_cb == NULL || step->timeout_ms == 0U ||
        s_node_id == 0xFFU) {
        return -EINVAL;
    }
    if (nrf_cs_pair_runtime_zephyr_is_busy()) {
        /* A disconnect the controller refused twice leaves CLEANUP owning a live link: ask the worker for one more try. */
        if (s_outgoing_conn != NULL && nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP &&
            atomic_get(&s_disconnect_retry_pending) != 0) {
            atomic_set(&s_disconnect_retry_request, 1);
            (void)submit_cs_work(&s_event_work);
        }
        return -EBUSY;
    }
    if (step->initiator != s_node_id) {
        return -EPERM;
    }

    k_spinlock_key_t cancel_key = k_spin_lock(&s_cancel_lock);
    if (s_cancel_pending && !nrf_cs_pair_runtime_bridge_steps_equal(&s_cancel_step, step)) {
        s_cancel_pending = false;
    }
    k_spin_unlock(&s_cancel_lock, cancel_key);
    s_user_complete_cb = complete_cb;
    s_user_complete_context = context;
    s_timeout_step = *step;
    s_attempt_deadline_ms = k_uptime_get() + (int64_t)step->timeout_ms;
    nrf_cs_trace_zephyr_execute(step->attempt_id, (uint8_t)step->initiator, (uint8_t)step->reflector,
        (uint8_t)step->attempt_number, (uint8_t)step->pair_index, step->timeout_ms);
    atomic_set(&s_execute_pending, 1);
    submit_result = submit_cs_work(&s_execute_work);
    if (submit_result < 0) {
        atomic_set(&s_execute_pending, 0);
        s_user_complete_cb = NULL;
        s_user_complete_context = NULL;
        return submit_result;
    }

    return 0;
}

bool nrf_cs_pair_runtime_zephyr_incoming_idle_ms(uint32_t *idle_ms)
{
    /* Only a link that is down and fully handled counts as silent; while one is up, or its disconnect is still queued
     * for the worker, the release reason may yet arrive. */
    if (idle_ms == NULL || atomic_get(&s_incoming_activity_seen) == 0 || s_incoming_conn != NULL ||
        atomic_get(&s_incoming_disconnect_pending) != 0) {
        return false;
    }
    *idle_ms = k_uptime_get_32() - (uint32_t)atomic_get(&s_incoming_activity_ms);
    return true;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return atomic_get(&s_execute_pending) != 0 || nrf_cs_pair_adapter_is_active(&s_adapter) ||
        s_incoming_conn != NULL || atomic_get(&s_incoming_disconnect_pending) != 0;
}

bool nrf_cs_pair_runtime_zephyr_is_progress_required(void)
{
    return atomic_get(&s_execute_pending) != 0 || nrf_cs_pair_adapter_is_active(&s_adapter) ||
        atomic_get(&s_reflector_procedure_active) != 0 || atomic_get(&s_incoming_disconnect_pending) != 0;
}
