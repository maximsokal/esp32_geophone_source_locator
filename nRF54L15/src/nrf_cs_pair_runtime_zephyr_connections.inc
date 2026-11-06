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
            /* Cancelled creates complete through connected(err), without disconnected(). */
            runtime_post_event(RUNTIME_EVENT_DISCONNECTED, &step, 0U, (int)err, NULL);
            return;
        }

        s_outgoing_connected = true;
        if (nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
            /* Creation may win the race with cancellation; release the live link too. */
            (void)op_cleanup(NULL);
            return;
        }
        runtime_post_event(RUNTIME_EVENT_CONNECTED, &step, 0U, 0, NULL);
        return;
    }

    if (err != 0U) {
        return;
    }
    if (atomic_get(&s_radio_enabled) == 0 || atomic_get(&s_incoming_disconnect_pending) != 0 ||
        nrf_cs_pair_adapter_is_active(&s_adapter) || s_incoming_conn != NULL) {
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    s_advertising = false;
    s_incoming_conn = bt_conn_ref(conn);
    atomic_set(&s_incoming_activity_ms, (atomic_val_t)k_uptime_get_32());
    atomic_set(&s_incoming_activity_seen, 1);
    runtime_post_event(RUNTIME_EVENT_INCOMING_CONNECTED, NULL, 0U, 0, conn);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    nrf_cs_step_t step;

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
        atomic_set(&s_incoming_disconnect_pending, (atomic_val_t)reason + 1);
        atomic_set(&s_incoming_activity_ms, (atomic_val_t)k_uptime_get_32());
        bt_conn_unref(s_incoming_conn);
        s_incoming_conn = NULL;
        atomic_set(&s_reflector_procedure_active, 0);
        /* The reason is the initiator's: NRF_CS_CYCLE_RELEASE_REASON means "that was my last attempt here". */
        runtime_post_event(RUNTIME_EVENT_INCOMING_DISCONNECTED, NULL, reason, (int)reason, NULL);
    }
}

