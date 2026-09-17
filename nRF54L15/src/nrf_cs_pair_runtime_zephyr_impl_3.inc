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

static void event_work_handler(struct k_work *work)
{
    runtime_event_t event;

    ARG_UNUSED(work);
    while (k_msgq_get(&s_event_queue, &event, K_NO_WAIT) == 0) {
        if (event.has_step && !runtime_step_matches_active(&event.step)) {
            if (event.conn != NULL) {
                bt_conn_unref(event.conn);
            }
            continue;
        }

        switch (event.type) {
        case RUNTIME_EVENT_CONNECTED:
            (void)nrf_cs_pair_adapter_connected(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_SECURITY_READY:
            (void)nrf_cs_pair_adapter_security_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_MTU_READY:
            (void)nrf_cs_pair_adapter_mtu_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_DISCOVERY_READY:
            (void)nrf_cs_pair_adapter_discovery_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_FEATURES_READY:
            (void)nrf_cs_pair_adapter_features_ready(
                &s_adapter,
                &event.step,
                (event.value & RAS_FEAT_REALTIME_RD) != 0U);
            break;
        case RUNTIME_EVENT_CAPABILITIES_READY:
            (void)nrf_cs_pair_adapter_capabilities_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_CONFIG_READY:
            (void)nrf_cs_pair_adapter_config_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_CS_SECURITY_READY:
            (void)nrf_cs_pair_adapter_cs_security_ready(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_TRANSPORT_ERROR:
            (void)nrf_cs_pair_adapter_transport_error(&s_adapter, &event.step);
            break;
        case RUNTIME_EVENT_DISCONNECTED:
            if (nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
                (void)nrf_cs_pair_adapter_cleanup_complete(&s_adapter, &event.step);
            } else {
                (void)nrf_cs_pair_adapter_transport_error(&s_adapter, &event.step);
            }
            break;
        case RUNTIME_EVENT_LOCAL_COMPLETE:
            s_local_counter = (uint16_t)event.value;
            s_local_complete = true;
            maybe_request_on_demand_data();
            maybe_complete_measurement(&event.step);
            break;
        case RUNTIME_EVENT_PEER_COMPLETE:
            s_peer_counter = (uint16_t)event.value;
            s_peer_complete = true;
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
            (void)nrf_cs_pair_adapter_measurement_ready(
                &s_adapter,
                &event.step,
                &measurement);
            break;
        }
        case RUNTIME_EVENT_INCOMING_CONNECTED:
            configure_reflector_connection(event.conn);
            break;
        case RUNTIME_EVENT_INCOMING_DISCONNECTED:
            (void)runtime_start_advertising();
            break;
        case RUNTIME_EVENT_REFLECTOR_CONFIG:
            configure_reflector_procedure(event.conn, (uint8_t)event.value);
            break;
        default:
            break;
        }

        if (event.conn != NULL) {
            bt_conn_unref(event.conn);
        }
    }
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    nrf_cs_step_t step;

    if (conn == s_outgoing_conn) {
        if (!runtime_active_step(&step)) {
            return;
        }
        if (err != 0U) {
            bt_conn_unref(s_outgoing_conn);
            s_outgoing_conn = NULL;
            s_outgoing_connected = false;
            runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, (int)err, NULL);
            return;
        }

        s_outgoing_connected = true;
        runtime_post_event(RUNTIME_EVENT_CONNECTED, &step, 0U, 0, NULL);
        return;
    }

    if (err != 0U) {
        return;
    }
    if (nrf_cs_pair_adapter_is_active(&s_adapter) || s_incoming_conn != NULL) {
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    s_advertising = false;
    s_incoming_conn = bt_conn_ref(conn);
    runtime_post_event(RUNTIME_EVENT_INCOMING_CONNECTED, NULL, 0U, 0, conn);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    nrf_cs_step_t step;

    ARG_UNUSED(reason);

    if (conn == s_outgoing_conn) {
        bool has_step = runtime_active_step(&step);
        bt_conn_unref(s_outgoing_conn);
        s_outgoing_conn = NULL;
        s_outgoing_connected = false;
        if (has_step) {
            runtime_post_event(RUNTIME_EVENT_DISCONNECTED, &step, 0U, 0, NULL);
        } else {
            (void)runtime_start_advertising();
        }
        return;
    }

    if (conn == s_incoming_conn) {
        bt_conn_unref(s_incoming_conn);
        s_incoming_conn = NULL;
        runtime_post_event(RUNTIME_EVENT_INCOMING_DISCONNECTED, NULL, 0U, 0, NULL);
    }
}

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

    if (!runtime_active_step_for_conn(conn, &step)) {
        return;
    }
    if (status != BT_HCI_ERR_SUCCESS) {
        runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, (int)status, NULL);
        return;
    }

    ARG_UNUSED(params);
}

static void subevent_data_cb(
    struct bt_conn *conn,
    struct bt_conn_le_cs_subevent_result *result)
{
    nrf_cs_step_t step;
    uint16_t ranging_counter;
    size_t len;

    if (!runtime_active_step_for_conn(conn, &step) || result == NULL) {
        return;
    }

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
        runtime_post_event(
            RUNTIME_EVENT_PROCEDURE_ABORTED,
            &step,
            ranging_counter,
            -ECANCELED,
            NULL);
    } else if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
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

int nrf_cs_pair_runtime_zephyr_start(void)
{
    int err;

    if (s_started) {
        return -EALREADY;
    }

    k_msgq_purge(&s_event_queue);
    k_work_init(&s_event_work, event_work_handler);
    k_work_init(&s_cleanup_work, cleanup_work_handler);
    k_work_init_delayable(&s_timeout_work, timeout_work_handler);
    runtime_reset_ranging();

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
    if (!s_started || node_id == 0xFFU) {
        return -EINVAL;
    }
    if (s_node_id != 0xFFU && s_node_id != node_id) {
        return -EALREADY;
    }

    s_node_id = node_id;
    s_service_data[2] = node_id;
    return runtime_start_advertising();
}

int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context)
{
    nrf_cs_pair_adapter_result_t result;

    if (!s_started || step == NULL || complete_cb == NULL ||
        s_node_id == 0xFFU) {
        return -EINVAL;
    }
    if (s_incoming_conn != NULL || nrf_cs_pair_adapter_is_active(&s_adapter)) {
        return -EBUSY;
    }
    if (step->initiator != s_node_id) {
        return -EPERM;
    }

    s_user_complete_cb = complete_cb;
    s_user_complete_context = context;
    s_timeout_step = *step;

    result = nrf_cs_pair_adapter_execute(
        &s_adapter,
        s_node_id,
        step,
        runtime_adapter_complete,
        NULL);
    if (result != NRF_CS_PAIR_ADAPTER_OK) {
        s_user_complete_cb = NULL;
        s_user_complete_context = NULL;
        return result == NRF_CS_PAIR_ADAPTER_ERR_BUSY ? -EBUSY : -EINVAL;
    }

    (void)k_work_reschedule(&s_timeout_work, K_MSEC(step->timeout_ms));
    return 0;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return nrf_cs_pair_adapter_is_active(&s_adapter) || s_incoming_conn != NULL;
}
