static int op_configure_ranging_data(void *context, bool realtime)
{
    int err;

    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }

    s_realtime_ras = realtime;
    if (realtime) {
        return bt_ras_rreq_realtime_rd_subscribe(
            s_outgoing_conn,
            &s_peer_rx,
            ranging_data_received_cb);
    }

    err = bt_ras_rreq_rd_overwritten_subscribe(
        s_outgoing_conn,
        ranging_data_overwritten_cb);
    if (err != 0) {
        return err;
    }
    err = bt_ras_rreq_rd_ready_subscribe(
        s_outgoing_conn,
        ranging_data_ready_cb);
    if (err != 0) {
        return err;
    }
    err = bt_ras_rreq_on_demand_rd_subscribe(s_outgoing_conn);
    if (err != 0) {
        return err;
    }
    return bt_ras_rreq_cp_subscribe(s_outgoing_conn);
}

static int set_default_cs_settings(struct bt_conn *conn, bool initiator)
{
    const struct bt_le_cs_set_default_settings_param settings = {
        .enable_initiator_role = initiator,
        .enable_reflector_role = !initiator,
        .cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
        .max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
    };
    return bt_le_cs_set_default_settings(conn, &settings);
}

static int op_exchange_capabilities(void *context)
{
    int err;

    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    err = set_default_cs_settings(s_outgoing_conn, true);
    if (err != 0) {
        return err;
    }
    return bt_le_cs_read_remote_supported_capabilities(s_outgoing_conn);
}

static void fill_cs_config(struct bt_le_cs_create_config_params *params)
{
    memset(params, 0, sizeof(*params));
    params->id = NRF_CS_CONFIG_ID;
    params->mode = BT_CONN_LE_CS_MAIN_MODE_2_SUB_MODE_1;
    params->min_main_mode_steps = 2U;
    params->max_main_mode_steps = 5U;
    params->main_mode_repetition = 0U;
    params->mode_0_steps = 3U;
    params->role = BT_CONN_LE_CS_ROLE_INITIATOR;
    params->rtt_type = BT_CONN_LE_CS_RTT_TYPE_AA_ONLY;
    params->cs_sync_phy = BT_CONN_LE_CS_SYNC_1M_PHY;
    params->channel_map_repetition = 1U;
    params->channel_selection_type = BT_CONN_LE_CS_CHSEL_TYPE_3B;
    params->ch3c_shape = BT_CONN_LE_CS_CH3C_SHAPE_HAT;
    params->ch3c_jump = 2U;
    bt_le_cs_set_valid_chmap_bits(params->channel_map);
}

static int op_create_config(void *context)
{
    struct bt_le_cs_create_config_params params;

    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    fill_cs_config(&params);
    return bt_le_cs_create_config(
        s_outgoing_conn,
        &params,
        BT_LE_CS_CREATE_CONFIG_CONTEXT_LOCAL_AND_REMOTE);
}

static int op_enable_cs_security(void *context)
{
    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }
    return bt_le_cs_security_enable(s_outgoing_conn);
}

static void fill_procedure_params(
    struct bt_le_cs_set_procedure_parameters_param *params,
    uint8_t config_id,
    bool reflector)
{
    uint16_t desired_interval = reflector ? 10U : (s_realtime_ras ? 5U : 10U);

    memset(params, 0, sizeof(*params));
    params->config_id = config_id;
    params->max_procedure_len =
        (uint16_t)(s_conn_param.interval_max * 2U * (desired_interval - 1U));
    params->min_procedure_interval = reflector ? 1U : desired_interval;
    params->max_procedure_interval = reflector ? 100U : desired_interval;
    params->max_procedure_count = 1U;
    params->min_subevent_len = reflector ? 10000U : 16000U;
    params->max_subevent_len = reflector ? 75000U : 16000U;
    params->tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1;
    params->phy = BT_LE_CS_PROCEDURE_PHY_2M;
    params->tx_power_delta = 0x80;
    params->preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1;
    params->snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED;
    params->snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED;
}

static int op_start_procedure(void *context)
{
    struct bt_le_cs_set_procedure_parameters_param procedure;
    struct bt_le_cs_procedure_enable_param enable = {
        .config_id = NRF_CS_CONFIG_ID,
        .enable = 1,
    };
    int err;

    ARG_UNUSED(context);
    if (s_outgoing_conn == NULL) {
        return -ENOTCONN;
    }

    fill_procedure_params(&procedure, NRF_CS_CONFIG_ID, false);
    err = bt_le_cs_set_procedure_parameters(s_outgoing_conn, &procedure);
    if (err != 0) {
        return err;
    }
    return bt_le_cs_procedure_enable(s_outgoing_conn, &enable);
}

static int op_cleanup(void *context)
{
    int err;

    ARG_UNUSED(context);
    runtime_stop_scan();

    if (s_outgoing_conn == NULL) {
        atomic_set(&s_disconnect_retry_pending, 0);
        if (submit_cs_work(&s_cleanup_work) < 0) cleanup_work_handler(NULL);
        return 0;
    }

    /* begin_terminal() stored the outcome before calling us: if it also ends the execution, say so in the reason. */
    uint8_t reason = BT_HCI_ERR_REMOTE_USER_TERM_CONN;
    if (s_relay_final_hint_cb != NULL && s_adapter.terminal_pending &&
        s_relay_final_hint_cb(&s_adapter.terminal_measurement)) {
        reason = NRF_CS_CYCLE_RELEASE_REASON;
    }
    err = bt_conn_disconnect(s_outgoing_conn, reason);
    if (err == 0 || err == -EALREADY) {
        atomic_set(&s_disconnect_retry_pending, 0);
        return 0;
    }
    if (err != -ENOTCONN) {
        atomic_set(&s_disconnect_retry_pending, 1);
        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED,
            s_adapter.step.attempt_id, (uint32_t)err);
        return err;
    }

    bt_conn_unref(s_outgoing_conn);
    s_outgoing_conn = NULL;
    s_outgoing_connected = false;
    atomic_set(&s_disconnect_retry_pending, 0);
    if (submit_cs_work(&s_cleanup_work) < 0) cleanup_work_handler(NULL);
    return 0;
}

static uint32_t op_now_ms(void *context)
{
    ARG_UNUSED(context);
    return k_uptime_get_32();
}

static const nrf_cs_pair_adapter_ops_t s_adapter_ops = {
    .connect = op_connect,
    .secure = op_secure,
    .exchange_mtu = op_exchange_mtu,
    .discover_ras = op_discover_ras,
    .read_features = op_read_features,
    .configure_ranging_data = op_configure_ranging_data,
    .exchange_capabilities = op_exchange_capabilities,
    .create_config = op_create_config,
    .enable_cs_security = op_enable_cs_security,
    .start_procedure = op_start_procedure,
    .cleanup = op_cleanup,
    .now_ms = op_now_ms,
    .context = NULL,
};

static void runtime_emit_rf_diagnostics(const nrf_cs_step_t *step)
{
    const uint32_t status = (uint32_t)atomic_get(&s_rf_status);
    const uint8_t flags = (uint8_t)atomic_get(&s_rf_flags) | s_rf_diagnostics.flags;
    const uint32_t signal_word = nrf_cs_rf_signal_word(&s_rf_diagnostics, (int8_t)atomic_get(&s_rf_tx_power));
    const uint32_t quality_word = nrf_cs_rf_quality_word(&s_rf_diagnostics, (uint8_t)(status & 15U), flags);
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_RF_SIGNAL, step->attempt_id, signal_word);
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_RF_QUALITY, step->attempt_id, quality_word);
    nrf_cs_trace_zephyr_rf(step->attempt_id, signal_word, quality_word);
    if ((flags & (NRF_CS_RF_ABORTED | NRF_CS_RF_ANTENNA_MISMATCH)) != 0U) {
        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_RF_STATUS, step->attempt_id, status);
    }
}

static void runtime_adapter_complete(
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement,
    void *context)
{
    nrf_cs_pair_runtime_complete_cb_t callback = s_user_complete_cb;
    void *callback_context = s_user_complete_context;

    (void)k_work_cancel_delayable(&s_timeout_work);
    s_user_complete_cb = NULL;
    s_user_complete_context = NULL;
    if (step != NULL && measurement != NULL) {
        nrf_cs_pair_adapter_timing_t timing = {0};
        /* Early queue failures have no adapter timing; its cached result belongs to the previous attempt. */
        if (context == &s_adapter) {
            (void)nrf_cs_pair_adapter_last_timing(&s_adapter, &timing);
        }
        timing.valid = true;
        /* Include queue wait even when the attempt never reached adapter_execute. */
        timing.total_ms = (uint32_t)(k_uptime_get() - (s_attempt_deadline_ms - step->timeout_ms));
        timing.measured_ms = timing.total_ms - timing.cleanup_ms;
        timing.budget_exceeded = timing.total_ms > step->timeout_ms;
        nrf_cs_trace_zephyr_result(step->attempt_id, (uint8_t)measurement->outcome, measurement->distance_mm,
            measurement->confidence, (uint8_t)nrf_cs_pair_adapter_phase(&s_adapter), &timing);
    }
    runtime_emit_rf_diagnostics(step);
    runtime_reset_ranging();
    (void)runtime_start_advertising();

    if (callback != NULL) {
        callback(step, measurement, callback_context);
    }
}

static void timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    nrf_cs_trace_zephyr_timeout(s_timeout_step.attempt_id, (uint8_t)nrf_cs_pair_adapter_phase(&s_adapter),
        s_timeout_step.timeout_ms);
    nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_CS_PAIR_PHASE,
        s_timeout_step.attempt_id, (uint32_t)nrf_cs_pair_adapter_phase(&s_adapter));
    (void)nrf_cs_pair_runtime_bridge_dispatch(
        &s_adapter, NRF_CS_PAIR_BRIDGE_TIMEOUT, &s_timeout_step, false, NULL);
    /* A full event queue can lose DISCONNECTED after its callback already released the link. */
    if (runtime_step_matches_active(&s_timeout_step) &&
        nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP &&
        s_outgoing_conn == NULL) {
        cleanup_work_handler(NULL);
        return;
    }
    /* Retry a rejected disconnect once at the existing deadline; keep ownership if it still fails. */
    if (nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP &&
        atomic_cas(&s_disconnect_retry_pending, 1, 0)) {
        (void)op_cleanup(NULL);
    }
}

static void cleanup_work_handler(struct k_work *work)
{
    nrf_cs_step_t step;

    ARG_UNUSED(work);
    if (runtime_active_step(&step) &&
        nrf_cs_pair_adapter_phase(&s_adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
        (void)nrf_cs_pair_runtime_bridge_dispatch(
            &s_adapter, NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE, &step, false, NULL);
    }
}

static bool report_ranging_header(struct ras_ranging_header *header, void *user_data)
{
    cs_de_report_t *report = user_data;

    s_rf_diagnostics.peer_tx_dbm = header->selected_tx_power;
    if (header->antenna_paths_mask != 1U) s_rf_diagnostics.flags |= NRF_CS_RF_ANTENNA_MISMATCH;
    report->n_ap = 1U;
    report->role = BT_CONN_LE_CS_ROLE_INITIATOR;
    return true;
}

static void accumulate_mean(float *mean, float value, uint16_t count)
{
    float alpha = 1.0f / (float)count;
    *mean = alpha * value + (1.0f - alpha) * (*mean);
}

static void collect_pbr_step(
    cs_de_report_t *report,
    const struct bt_le_cs_subevent_step *local_step,
    const struct bt_le_cs_subevent_step *peer_step)
{
    const struct bt_hci_le_cs_step_data_mode_2 *local;
    const struct bt_hci_le_cs_step_data_mode_2 *peer;
    struct bt_le_cs_iq_sample local_iq;
    struct bt_le_cs_iq_sample peer_iq;
    uint8_t channel_index;
    uint16_t count;

    if (local_step->channel < NRF_CS_CHANNEL_INDEX_OFFSET ||
        local_step->channel >= (NRF_CS_CHANNEL_INDEX_OFFSET + CS_DE_NUM_CHANNELS)) {
        return;
    }

    local = (const struct bt_hci_le_cs_step_data_mode_2 *)local_step->data;
    peer = (const struct bt_hci_le_cs_step_data_mode_2 *)peer_step->data;
    nrf_cs_rf_tone(&s_rf_diagnostics.local, local->tone_info[0].quality_indicator);
    nrf_cs_rf_tone(&s_rf_diagnostics.peer, peer->tone_info[0].quality_indicator);

    if (local->tone_info[0].quality_indicator != BT_HCI_LE_CS_TONE_QUALITY_HIGH ||
        peer->tone_info[0].quality_indicator != BT_HCI_LE_CS_TONE_QUALITY_HIGH) {
        return;
    }

    channel_index = (uint8_t)(local_step->channel - NRF_CS_CHANNEL_INDEX_OFFSET);
    local_iq = bt_le_cs_parse_pct(local->tone_info[0].phase_correction_term);
    peer_iq = bt_le_cs_parse_pct(peer->tone_info[0].phase_correction_term);
    count = ++s_iq_counts[channel_index];

    /* Cancel each exchange's oscillator phase before averaging repeated tones.
     * The SDK multiplies the two report sides: unity preserves the combined mean. */
    const float combined_i = (float)local_iq.i * peer_iq.i - (float)local_iq.q * peer_iq.q;
    const float combined_q = (float)local_iq.i * peer_iq.q + (float)local_iq.q * peer_iq.i;
    report->iq_tones[0].i_remote[channel_index] = 1.0f;
    report->iq_tones[0].q_remote[channel_index] = 0.0f;

    if (count == 1U) {
        report->iq_tones[0].i_local[channel_index] = combined_i;
        report->iq_tones[0].q_local[channel_index] = combined_q;
        return;
    }

    accumulate_mean(&report->iq_tones[0].i_local[channel_index], combined_i, count);
    accumulate_mean(&report->iq_tones[0].q_local[channel_index], combined_q, count);
}

static void collect_rtt_step(
    cs_de_report_t *report,
    const struct bt_le_cs_subevent_step *local_step,
    const struct bt_le_cs_subevent_step *peer_step)
{
    const struct bt_hci_le_cs_step_data_mode_1 *local =
        (const struct bt_hci_le_cs_step_data_mode_1 *)local_step->data;
    const struct bt_hci_le_cs_step_data_mode_1 *peer =
        (const struct bt_hci_le_cs_step_data_mode_1 *)peer_step->data;
    nrf_cs_rf_packet(&s_rf_diagnostics.local, (int8_t)local->packet_rssi, local->packet_quality_aa_check);
    nrf_cs_rf_packet(&s_rf_diagnostics.peer, (int8_t)peer->packet_rssi, peer->packet_quality_aa_check);
    if ((local->packet_antenna != 1U && local->packet_antenna != 0xffU) ||
        (peer->packet_antenna != 1U && peer->packet_antenna != 0xffU)) {
        s_rf_diagnostics.flags |= NRF_CS_RF_ANTENNA_MISMATCH;
    }

    if (local->packet_quality_aa_check != BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
        peer->packet_quality_aa_check != BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
        local->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
        peer->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
        local->toa_tod_initiator == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE ||
        peer->tod_toa_reflector == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE) {
        return;
    }

    report->rtt_accumulated_half_ns +=
        local->toa_tod_initiator - peer->tod_toa_reflector;
    report->rtt_count++;
}

static bool report_step_data(
    struct bt_le_cs_subevent_step *local_step,
    struct bt_le_cs_subevent_step *peer_step,
    void *user_data)
{
    cs_de_report_t *report = user_data;

    if (local_step == NULL || peer_step == NULL || report == NULL) {
        return false;
    }
    size_t minimum = 0U;
    if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_2) {
        minimum = sizeof(struct bt_hci_le_cs_step_data_mode_2) + sizeof(struct bt_hci_le_cs_step_data_tone_info);
    } else if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_1) {
        minimum = sizeof(struct bt_hci_le_cs_step_data_mode_1);
    } else if (local_step->mode == 0U) {
        /* Initiator mode0 adds frequency offset after this common three-byte prefix. */
        minimum = sizeof(struct bt_hci_le_cs_step_data_mode_0_reflector);
    }
    if (local_step->data == NULL || peer_step->data == NULL || minimum == 0U ||
        local_step->mode != peer_step->mode || local_step->channel != peer_step->channel ||
        local_step->data_len < minimum || peer_step->data_len < minimum) {
        s_rf_diagnostics.flags |= NRF_CS_RF_MALFORMED;
        return false;
    }

    if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_2) {
        collect_pbr_step(report, local_step, peer_step);
    } else if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_1) {
        collect_rtt_step(report, local_step, peer_step);
    } else {
        const struct bt_hci_le_cs_step_data_mode_0_reflector *local = (const void *)local_step->data;
        const struct bt_hci_le_cs_step_data_mode_0_reflector *peer = (const void *)peer_step->data;
        nrf_cs_rf_packet(&s_rf_diagnostics.local, (int8_t)local->packet_rssi, local->packet_quality_aa_check);
        nrf_cs_rf_packet(&s_rf_diagnostics.peer, (int8_t)peer->packet_rssi, peer->packet_quality_aa_check);
    }
    return true;
}

/* Which of the 75 slots hold a usable tone, and how long the longest hole is (nrf_cs_tone_stats.h). */
static nrf_cs_tone_stats_t current_tone_stats(void)
{
    bool planned[CS_DE_NUM_CHANNELS];
    bool usable[CS_DE_NUM_CHANNELS];

    for (uint8_t channel = 0U; channel < CS_DE_NUM_CHANNELS; ++channel) {
        planned[channel] = BT_LE_CS_CHANNEL_BIT_GET(s_cs_config.channel_map, channel + NRF_CS_CHANNEL_INDEX_OFFSET);
        usable[channel] = s_iq_counts[channel] > 0U;
    }
    return nrf_cs_tone_stats(planned, usable, CS_DE_NUM_CHANNELS);
}

static uint16_t good_tone_channel_count(void)
{
    uint16_t count = 0U;
    uint8_t channel;

    for (channel = 0U; channel < CS_DE_NUM_CHANNELS; ++channel) {
        if (BT_LE_CS_CHANNEL_BIT_GET(
                s_cs_config.channel_map,
                channel + NRF_CS_CHANNEL_INDEX_OFFSET) &&
            s_iq_counts[channel] > 0U) {
            count++;
        }
    }
    return count;
}

static nrf_cs_measurement_t estimate_measurement(void)
{
    nrf_cs_measurement_t measurement = {
        .outcome = NRF_CS_OUTCOME_OUTLIER,
        .distance_mm = 0,
        .confidence = 0U,
        .provider_quality_flags = 0U,
    };
    cs_de_quality_t quality;
    float best;
    double mm;

    memset(&s_de_report, 0, sizeof(s_de_report));
    memset(s_iq_counts, 0, sizeof(s_iq_counts));
    s_de_report.role = BT_CONN_LE_CS_ROLE_INITIATOR;
    s_de_report.n_ap = 1U;
    s_de_report.distance_estimates[0].ifft = NAN;
    s_de_report.distance_estimates[0].phase_slope = NAN;
    s_de_report.distance_estimates[0].rtt = NAN;
    s_de_report.distance_estimates[0].best = NAN;

    bt_ras_rreq_rd_subevent_data_parse(
        &s_peer_owned,
        &s_local_steps,
        BT_CONN_LE_CS_ROLE_INITIATOR,
        report_ranging_header,
        NULL,
        report_step_data,
        &s_de_report);

    const uint16_t good_tones = good_tone_channel_count();
    s_rf_diagnostics.good_channels = (uint8_t)good_tones;
    s_de_report.tone_quality[0] =
        good_tones >= NRF_CS_TONE_OK_THRESHOLD ? CS_DE_TONE_QUALITY_OK : CS_DE_TONE_QUALITY_BAD;

    /* A usable prefix must not hide a malformed tail or unsupported antenna mapping. */
    if (((s_rf_diagnostics.flags | (uint8_t)atomic_get(&s_rf_flags)) &
            (NRF_CS_RF_MALFORMED | NRF_CS_RF_ANTENNA_MISMATCH)) != 0U) {
        return measurement;
    }

    quality = cs_de_calc(&s_de_report);
    const float stock_ifft_m = s_de_report.distance_estimates[0].ifft;     /* the library's own, before the gap fill */
    uint16_t peak_floor_x10 = 0U;
    float peak_floor_ratio = NAN;
    if (s_de_report.tone_quality[0] == CS_DE_TONE_QUALITY_OK &&
        isfinite(s_de_report.distance_estimates[0].ifft) &&
        s_de_report.distance_estimates[0].best == s_de_report.distance_estimates[0].ifft) {
        memset(s_iq_ifft_scratch, 0, sizeof(s_iq_ifft_scratch));
        cs_de_combined_iq_calculate(&s_de_report.iq_tones[0], s_iq_ifft_scratch);
        const bool gaps_filled = nrf_cs_iq_gap_fill(s_iq_ifft_scratch, s_iq_counts, CS_DE_NUM_CHANNELS) > 0U;
        /* The call also leaves the IFFT magnitude profile in the first NFFT floats: the sharpness of the peak is one pass over it.
         * The distance of a set without holes is the library's own, so only a filled set replaces it. */
        const float checked_ifft = cs_de_ifft(s_iq_ifft_scratch);
        if (gaps_filled && isfinite(checked_ifft) && checked_ifft >= 0.0f) {
            s_de_report.distance_estimates[0].ifft = checked_ifft;
            s_de_report.distance_estimates[0].best = checked_ifft;
        }
        const float ratio = nrf_cs_ifft_peak_to_floor(s_iq_ifft_scratch, CONFIG_BT_CS_DE_NFFT_SIZE);
        if (isfinite(ratio) && ratio > 0.0f) {
            peak_floor_ratio = ratio;
            const float scaled = ratio * 10.0f + 0.5f;
            peak_floor_x10 = scaled >= 65535.0f ? UINT16_MAX : (uint16_t)scaled;
        }
    }
    /* NCS 3.4 skips best selection when PBR tones are bad, even if RTT is available. */
    if (quality == CS_DE_QUALITY_OK && s_de_report.tone_quality[0] == CS_DE_TONE_QUALITY_BAD &&
        !isfinite(s_de_report.distance_estimates[0].best) && isfinite(s_de_report.distance_estimates[0].rtt) &&
        s_de_report.distance_estimates[0].rtt > 0.0f) {
        s_de_report.distance_estimates[0].best = s_de_report.distance_estimates[0].rtt;
    }
    /* The SDK can also choose its clamped zero RTT as best when both PBR methods failed. */
    if (!isfinite(s_de_report.distance_estimates[0].ifft) &&
        !isfinite(s_de_report.distance_estimates[0].phase_slope) &&
        (!isfinite(s_de_report.distance_estimates[0].rtt) || s_de_report.distance_estimates[0].rtt <= 0.0f)) {
        s_de_report.distance_estimates[0].best = NAN;
    }
    /* The factor that decides the confidence, in the log next to the estimates it is made of. */
    const char *const why = nrf_cs_confidence_reason_name(nrf_cs_measurement_confidence_reason(
        good_tones, NRF_CS_TONE_OK_THRESHOLD,
        isfinite(s_de_report.distance_estimates[0].ifft) || isfinite(s_de_report.distance_estimates[0].phase_slope),
        isfinite(s_de_report.distance_estimates[0].rtt) && s_de_report.distance_estimates[0].rtt > 0.0f,
        s_de_report.distance_estimates[0].best, s_de_report.distance_estimates[0].rtt, peak_floor_ratio));
    const nrf_cs_tone_stats_t tone_stats = current_tone_stats();
    nrf_cs_trace_zephyr_estimates(s_timeout_step.attempt_id, s_de_report.distance_estimates[0].ifft,
        s_de_report.distance_estimates[0].phase_slope, s_de_report.distance_estimates[0].rtt,
        s_de_report.distance_estimates[0].best, tone_stats.planned, tone_stats.usable, tone_stats.longest_gap,
        tone_stats.lead_gap, tone_stats.tail_gap, peak_floor_x10, stock_ifft_m, why);

    /* CS-ESTIMATOR-01: record which estimate "best" came from, in the attempt record the ESP32 reads. */
    measurement.provider_quality_flags |= nrf_cs_distance_method_flag(nrf_cs_distance_method_of(
        nrf_cs_distance_mm_from_m(s_de_report.distance_estimates[0].ifft),
        nrf_cs_distance_mm_from_m(s_de_report.distance_estimates[0].phase_slope),
        nrf_cs_distance_mm_from_m(s_de_report.distance_estimates[0].rtt),
        nrf_cs_distance_mm_from_m(s_de_report.distance_estimates[0].best)));
    if (s_realtime_ras) {
        measurement.provider_quality_flags |= NRF_CS_PROVIDER_QUALITY_REALTIME_RAS;
    }
    if (s_de_report.tone_quality[0] == CS_DE_TONE_QUALITY_OK) {
        measurement.provider_quality_flags |= NRF_CS_PROVIDER_QUALITY_TONE_OK;
    }
    if (isfinite(s_de_report.distance_estimates[0].ifft) ||
        isfinite(s_de_report.distance_estimates[0].phase_slope)) {
        measurement.provider_quality_flags |= NRF_CS_PROVIDER_QUALITY_PBR_VALID;
    }
    /* cs_de_rtt clamps nonpositive time-of-flight to zero, which cannot corroborate PBR. */
    if (isfinite(s_de_report.distance_estimates[0].rtt) && s_de_report.distance_estimates[0].rtt > 0.0f) {
        measurement.provider_quality_flags |= NRF_CS_PROVIDER_QUALITY_RTT_VALID;
    }

    best = s_de_report.distance_estimates[0].best;
    mm = (double)best * 1000.0;
    if (quality != CS_DE_QUALITY_OK ||
        !isfinite(best) ||
        mm < 0.0 ||
        mm > (double)INT32_MAX) {
        return measurement;
    }

    measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    measurement.distance_mm = (int32_t)(mm + 0.5);
    /* MAX-206: evidence-based confidence (tone coverage, PBR/RTT agreement)
     * instead of a constant 1000 for every success. */
    measurement.confidence = nrf_cs_measurement_confidence_with_peak(
        good_tones,
        NRF_CS_TONE_OK_THRESHOLD,
        (measurement.provider_quality_flags & NRF_CS_PROVIDER_QUALITY_PBR_VALID) != 0U,
        (measurement.provider_quality_flags & NRF_CS_PROVIDER_QUALITY_RTT_VALID) != 0U,
        best,
        s_de_report.distance_estimates[0].rtt,
        peak_floor_ratio
    );
    return measurement;
}

static void maybe_request_on_demand_data(void)
{
    int err;

    if (s_realtime_ras || s_on_demand_request_started ||
        !s_local_complete ||
        s_ready_counter == NRF_CS_RANGING_COUNTER_NONE ||
        s_ready_counter != s_local_counter ||
        s_outgoing_conn == NULL) {
        return;
    }

    net_buf_simple_reset(&s_peer_rx);
    s_on_demand_request_started = true;
    err = bt_ras_rreq_cp_get_ranging_data(
        s_outgoing_conn,
        &s_peer_rx,
        s_ready_counter,
        ranging_data_received_cb);
    if (err != 0) {
        nrf_cs_step_t step;
        if (runtime_active_step(&step)) {
            runtime_post_event(RUNTIME_EVENT_TRANSPORT_ERROR, &step, 0U, err, NULL);
        }
    }
}

static void maybe_complete_measurement(const nrf_cs_step_t *step)
{
    nrf_cs_measurement_t measurement;

    if (step == NULL || !s_local_complete || !s_peer_complete) {
        return;
    }
    if (s_local_counter != s_peer_counter ||
        s_local_steps.len == 0U ||
        s_peer_owned.len == 0U) {
        measurement.outcome = NRF_CS_OUTCOME_OUTLIER;
        measurement.distance_mm = 0;
        measurement.confidence = 0U;
        measurement.provider_quality_flags =
            s_realtime_ras ? NRF_CS_PROVIDER_QUALITY_REALTIME_RAS : 0U;
    } else {
        measurement = estimate_measurement();
    }

    if (k_uptime_get() >= s_attempt_deadline_ms) {
        timeout_work_handler(NULL);
        return;
    }
    (void)nrf_cs_pair_adapter_measurement_ready(&s_adapter, step, &measurement);
}
