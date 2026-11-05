"""Execute exact production CS handler bodies with fake external APIs/clock.

This is a host seam test, not execution of Zephyr or the Nordic radio stack.
Source bodies are extracted unchanged; their digests remain beside the result.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("mutation", ["production", "missing-rf-summary", "missing-method", "missing-queue-timing", "stale-timing-context", "disabled-is-enabled", "unfrozen-local", "unfrozen-peer", "missing-radio-stop", "missing-radio-disconnect", "missing-cancel", "foreign-cancel", "queued-cancel-starts", "cancel-not-submitted", "forget-live-disconnect", "cleanup-no-fallback", "no-disconnect-retry", "normal-disconnect-releases", "lost-disconnect-no-deadline", "lost-disconnect-no-worker", "lost-incoming-no-mailbox", "lost-incoming-no-release", "lost-incoming-not-cleared", "lost-incoming-new-owner", "forget-advertising-stop", "scan-after-stop-error", "procedure-loses-config", "cold-reset-expects-zero", "local-counter-unchecked", "peer-counter-unchecked", "ready-counter-unchecked", "warm-reset-active", "warm-reset-unfrozen", "warm-reset-same-counter", "sdk-ras", "sdk-rx-reset", "sdk-early-idle", "sdk-ignore-get-error", "sdk-disconnect-no-free", "sdk-alloc-no-reset", "payload-counter-unchecked", "payload-config-unchecked", "local-config-unchecked", "enable-config-unchecked", "phase-check-bypass", "phase-local-bypass", "phase-enable-bypass", "phase-peer-bypass", "phase-ready-bypass", "phase-overwritten-bypass", "overflow-error-not-saved", "overflow-error-not-drained", "measurement-commit-error-bypass", "overflow-error-overwrite-first", "overflow-error-foreign", "overflow-error-no-wakeup"])
def test_production_cs_handlers_enforce_deadline_and_reject_foreign_steps(mutation):
    build_root = ROOT / ".test-build"
    build_root.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="cs-handler-proof-", dir=build_root))
    parts = [ROOT / f"nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_{i}.inc" for i in (1, 2, 3)]
    parts.append(ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_connections.inc")
    sources = [p.read_text(encoding="utf-8") for p in parts]
    adapter_ops = sources[1].split("static const nrf_cs_pair_adapter_ops_t s_adapter_ops = {", 1)[1].split("};", 1)[0]
    assert ".now_ms = op_now_ms," in adapter_ops  # The target adapter must actually enable its clock.
    sdk_ras = mutation in ("sdk-ras", "sdk-rx-reset", "sdk-early-idle", "sdk-ignore-get-error", "sdk-disconnect-no-free", "sdk-alloc-no-reset")
    if sdk_ras:
        sdk = Path(os.environ.get("BLE6_NCS_ROOT", "C:/ncs/v3.4.0/nrf"))
        ras = sdk / "subsys/bluetooth/services/ras/rreq/ras_rreq.c"
        if not ras.is_file():
            pytest.skip("NCS RAS source unavailable")
        sdk_source = ras.read_text(encoding="utf-8")
        bodies = []
        for name in ("store_ranging_data_segment", "data_receive_finished", "bt_ras_rreq_cp_get_ranging_data", "handle_rsp_code", "ras_rreq_find", "ras_rreq_alloc", "bt_ras_rreq_free", "disconnected"):
            match = re.search(r"(?m)^(?:static\s+)?[\w* \t]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", sdk_source)
            start, end, depth = match.start(), match.end(), 1
            while depth:
                depth += (sdk_source[end] == "{") - (sdk_source[end] == "}")
                end += 1
            bodies.append(sdk_source[start:end])
        compiled_bodies = list(bodies)
        if mutation == "sdk-early-idle":
            call = "rreq->on_demand_rd.data_cb("
            assert call in compiled_bodies[1]
            compiled_bodies[1] = compiled_bodies[1].replace(call,
                "rreq->on_demand_rd.data_get_in_progress = false; " + call)
        if mutation == "sdk-disconnect-no-free":
            assert "bt_ras_rreq_free(conn);" in compiled_bodies[7]
            compiled_bodies[7] = compiled_bodies[7].replace("bt_ras_rreq_free(conn);", "(void)conn;")
        if mutation == "sdk-alloc-no-reset":
            assert "memset(rreq, 0, sizeof(struct bt_ras_rreq));" in compiled_bodies[5]
            compiled_bodies[5] = compiled_bodies[5].replace("memset(rreq, 0, sizeof(struct bt_ras_rreq));", "(void)rreq;")
        (output / "sdk_ras.inc").write_text("\n".join(compiled_bodies), encoding="utf-8")
        (output / "sdk_ras_provenance.json").write_text(json.dumps({
            "path": str(ras), "source_sha256": hashlib.sha256(ras.read_bytes()).hexdigest(),
            "function_sha256": [hashlib.sha256(b.encode()).hexdigest() for b in bodies],
            "compiled_sha256": [hashlib.sha256(b.encode()).hexdigest() for b in compiled_bodies]}, indent=2))
    provenance = []
    types = sources[0][sources[0].index("typedef enum {"):sources[0].index("K_MSGQ_DEFINE(")]
    (output / "production_types.inc").write_text(types, encoding="utf-8")
    for part, name in (
        (1, "runtime_post_event"), (1, "runtime_take_overflow_error"), (1, "runtime_active_procedure_for_conn"),
        (1, "runtime_stop_advertising"), (1, "op_connect"),
        (1, "runtime_reset_procedure"), (1, "runtime_reset_ranging"), (1, "runtime_counter_matches"),
        (1, "ranging_data_received_cb"), (1, "ranging_data_ready_cb"), (1, "ranging_data_overwritten_cb"),
        (1, "runtime_take_cancel"), (1, "nrf_cs_pair_runtime_zephyr_cancel"),
        (4, "connected_cb"), (4, "disconnected_cb"),
        (2, "runtime_emit_rf_diagnostics"), (2, "report_ranging_header"),
        (2, "accumulate_mean"), (2, "collect_pbr_step"),
        (2, "collect_rtt_step"), (2, "report_step_data"),
        (2, "current_tone_stats"), (2, "good_tone_channel_count"), (2, "estimate_measurement"),
        (2, "op_now_ms"), (2, "runtime_adapter_complete"), (2, "timeout_work_handler"),
        (2, "op_cleanup"),
        (2, "cleanup_work_handler"), (2, "maybe_complete_measurement"), (2, "maybe_request_on_demand_data"),
        (3, "event_work_handler"), (3, "execute_work_handler"),
        (3, "nrf_cs_pair_runtime_zephyr_execute"),
        (3, "nrf_cs_pair_runtime_zephyr_set_radio_enabled"),
        (3, "procedure_enable_complete_cb"), (3, "subevent_data_cb"),
        (3, "nrf_cs_pair_runtime_zephyr_is_progress_required"),
        (3, "nrf_cs_pair_runtime_zephyr_is_busy"),
    ):
        source = sources[part - 1]
        definition = re.search(r"(?m)^(?:static\s+)?[\w* \t]+\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
        assert definition is not None, name
        start = definition.start()
        brace = definition.end() - 1
        depth, end = 1, brace + 1
        # These selected definitions contain balanced braces in their comments/strings.
        while depth:
            if source[end] == "{":
                depth += 1
            elif source[end] == "}":
                depth -= 1
            end += 1
        body = source[start:end]
        original_digest = hashlib.sha256(body.encode()).hexdigest()
        if name == "runtime_post_event":
            if mutation == "overflow-error-not-saved":
                condition = "recover_terminal && event.has_step &&"
                assert condition in body
                body = body.replace(condition, "recover_terminal && event.status == 0 && event.has_step &&")
            if mutation == "overflow-error-overwrite-first":
                condition = "!nrf_cs_pair_runtime_bridge_steps_equal(&s_overflow_error.step, &event.step)"
                assert condition in body
                body = body.replace(condition, "event.status != 0")
            if mutation == "overflow-error-foreign":
                condition = "nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, &event.step)"
                assert condition in body
                body = body.replace(condition, "event.has_step")
            if mutation == "overflow-error-no-wakeup":
                condition = "type == RUNTIME_EVENT_INCOMING_DISCONNECTED || recover_terminal"
                assert condition in body
                body = body.replace(condition, "type == RUNTIME_EVENT_INCOMING_DISCONNECTED || (recover_terminal && event.status == 0)")
        if name == "event_work_handler" and mutation == "overflow-error-not-drained":
            condition = "runtime_take_overflow_error(&event) || k_msgq_get(&s_event_queue, &event, K_NO_WAIT) == 0"
            assert condition in body
            body = body.replace(condition, "(runtime_take_overflow_error(&event) && event.status == 0) || k_msgq_get(&s_event_queue, &event, K_NO_WAIT) == 0")
        if name == "maybe_complete_measurement" and mutation == "measurement-commit-error-bypass":
            condition = "atomic_cas(&s_measurement_state, RUNTIME_MEASUREMENT_OPEN, RUNTIME_MEASUREMENT_COMMITTED)"
            assert condition in body
            body = body.replace(condition, condition + " || atomic_get(&s_measurement_state) == RUNTIME_MEASUREMENT_FAILED")
        if mutation == "phase-check-bypass" and name == "runtime_active_procedure_for_conn":
            body = body.replace("== NRF_CS_PAIR_PHASE_PROCEDURE", "!= NRF_CS_PAIR_PHASE_IDLE")
        phase_callback = {"phase-local-bypass": "subevent_data_cb", "phase-enable-bypass": "procedure_enable_complete_cb",
                          "phase-peer-bypass": "ranging_data_received_cb", "phase-ready-bypass": "ranging_data_ready_cb",
                          "phase-overwritten-bypass": "ranging_data_overwritten_cb"}.get(mutation)
        if name == phase_callback:
            assert "runtime_active_procedure_for_conn(conn, &step)" in body
            body = body.replace("runtime_active_procedure_for_conn(conn, &step)", "runtime_active_step_for_conn(conn, &step)")
        if name == "report_ranging_header" and mutation in ("payload-counter-unchecked", "payload-config-unchecked"):
            condition = "header->ranging_counter != s_local_counter" if mutation == "payload-counter-unchecked" else "header->config_id != NRF_CS_CONFIG_ID"
            assert condition in body
            body = body.replace(condition, "(" + condition + " && s_local_counter == NRF_CS_RANGING_COUNTER_NONE)")
        if mutation == "local-config-unchecked" and name == "subevent_data_cb":
            condition = "result->header.config_id != NRF_CS_CONFIG_ID"
            assert condition in body
            body = body.replace(condition, condition + " && result->header.config_id == 255U")
        if mutation == "enable-config-unchecked" and name == "procedure_enable_complete_cb":
            condition = "params->config_id != NRF_CS_CONFIG_ID"
            assert condition in body
            body = body.replace(condition, condition + " && params->config_id == 255U")
        if mutation == "sdk-ignore-get-error" and name == "maybe_request_on_demand_data":
            assert "if (err != 0)" in body
            body = body.replace("if (err != 0)", "if (err > 0)")
        if mutation == "sdk-rx-reset" and name == "runtime_reset_procedure":
            body = body.replace("net_buf_simple_reset(&s_local_steps);",
                                "net_buf_simple_reset(&s_local_steps); net_buf_simple_reset(&s_peer_rx);")
        if name == "runtime_reset_procedure" and mutation in ("warm-reset-active", "warm-reset-unfrozen", "warm-reset-same-counter"):
            condition = {"warm-reset-active": "nrf_cs_pair_adapter_is_active(&s_adapter)",
                         "warm-reset-unfrozen": "atomic_get(&s_local_data_frozen) == 0",
                         "warm-reset-same-counter": "expected_counter == s_local_counter"}[mutation]
            assert condition in body
            body = body.replace(condition, "expected_counter == 65534U")
        if mutation == "procedure-loses-config" and name == "runtime_reset_procedure":
            assignment = "atomic_set(&s_expected_counter, expected_counter);"
            assert assignment in body
            body = body.replace(assignment, assignment + " memset(&s_cs_config, 0, sizeof(s_cs_config));")
        if mutation == "cold-reset-expects-zero" and name == "runtime_reset_ranging":
            call = "runtime_reset_procedure(NRF_CS_RANGING_COUNTER_NONE);"
            assert call in body
            body = body.replace(call, "runtime_reset_procedure(0U);")
        if mutation == "local-counter-unchecked" and name == "subevent_data_cb":
            condition = "conn == s_outgoing_conn && !runtime_counter_matches("
            assert condition in body
            body = body.replace(condition, "conn == s_outgoing_conn && false && !runtime_counter_matches(")
        if ((mutation == "peer-counter-unchecked" and name == "ranging_data_received_cb") or
                (mutation == "ready-counter-unchecked" and name == "ranging_data_ready_cb")):
            condition = "if (!runtime_counter_matches(ranging_counter))"
            assert condition in body
            body = body.replace(condition, "if (!runtime_counter_matches(ranging_counter) && ranging_counter == NRF_CS_RANGING_COUNTER_NONE)")
        if mutation == "forget-advertising-stop" and name == "runtime_stop_advertising":
            warning = 'LOG_WRN("RAS advertising stop failed: %d", err);'
            assert warning in body
            body = body.replace(warning, warning + " s_advertising = false;")
        if mutation == "scan-after-stop-error" and name == "op_connect":
            guard = "err = runtime_stop_advertising();\n    if (err != 0)"
            assert guard in body
            body = body.replace(guard, "err = runtime_stop_advertising();\n    if (err == 1)")
        if mutation == "lost-incoming-no-mailbox" and name == "disconnected_cb":
            assignment = "atomic_set(&s_incoming_disconnect_pending, (atomic_val_t)reason + 1);"
            assert assignment in body
            body = body.replace(assignment, "(void)s_incoming_disconnect_pending;")
        if mutation == "lost-incoming-no-release" and name == "event_work_handler":
            condition = "incoming_disconnect - 1 == NRF_CS_CYCLE_RELEASE_REASON"
            assert condition in body
            body = body.replace(condition, condition + " && false")
        if mutation == "lost-incoming-not-cleared" and name == "event_work_handler":
            assignment = "atomic_set(&s_incoming_disconnect_pending, 0);"
            assert assignment in body
            head, tail = body.rsplit(assignment, 1)
            body = head + "(void)s_incoming_disconnect_pending;" + tail
        if mutation == "lost-incoming-new-owner" and name == "connected_cb":
            condition = "atomic_get(&s_incoming_disconnect_pending) != 0"
            assert condition in body
            body = body.replace(condition, condition + " && false")
        if mutation == "lost-disconnect-no-deadline" and name == "timeout_work_handler":
            assert "s_outgoing_conn == NULL" in body
            body = body.replace("s_outgoing_conn == NULL", "s_outgoing_conn == NULL && false")
        if mutation == "lost-disconnect-no-worker" and name == "event_work_handler":
            assert "if (s_outgoing_conn == NULL &&" in body
            body = body.replace("if (s_outgoing_conn == NULL &&", "if (s_outgoing_conn == NULL && false &&")
        if mutation == "forget-live-disconnect" and name == "op_cleanup":
            assert "err != -ENOTCONN" in body
            body = body.replace("err != -ENOTCONN", "err != -ENOTCONN && err == -EINVAL")
        if mutation == "cleanup-no-fallback" and name == "op_cleanup":
            call = "if (submit_cs_work(&s_cleanup_work) < 0) cleanup_work_handler(NULL);"
            assert call in body
            body = body.replace(call, "(void)submit_cs_work(&s_cleanup_work);")
        if mutation == "no-disconnect-retry" and name == "timeout_work_handler":
            assert "atomic_cas(&s_disconnect_retry_pending, 1, 0)" in body
            body = body.replace("atomic_cas(&s_disconnect_retry_pending, 1, 0)", "atomic_cas(&s_disconnect_retry_pending, 2, 0)")
        if mutation == "normal-disconnect-releases" and name == "event_work_handler":
            assert "event.value == NRF_CS_CYCLE_RELEASE_REASON" in body
            body = body.replace("event.value == NRF_CS_CYCLE_RELEASE_REASON", "event.value != 0U")
        if mutation == "cancel-not-submitted" and name == "nrf_cs_pair_runtime_zephyr_cancel":
            body = body.replace("(void)submit_cs_work(&s_event_work);", "(void)s_event_work;")
        if mutation == "missing-cancel" and name == "runtime_take_cancel":
            body = body.replace("return cancelled;", "return cancelled && step == NULL;")
        if mutation == "foreign-cancel" and name == "runtime_take_cancel":
            body = body.replace("nrf_cs_pair_runtime_bridge_steps_equal(&s_cancel_step, step)", "step != NULL")
        if mutation == "foreign-cancel" and name == "nrf_cs_pair_runtime_zephyr_cancel":
            body = body.replace("!nrf_cs_pair_runtime_bridge_step_matches(&s_adapter, step)", "step == NULL")
        if mutation == "queued-cancel-starts" and name == "execute_work_handler":
            body = body.replace("runtime_take_cancel(&s_timeout_step)", "false")
        if mutation == "missing-radio-stop" and name == "nrf_cs_pair_runtime_zephyr_set_radio_enabled":
            call = "runtime_post_event(RUNTIME_EVENT_RADIO_DISABLED, NULL, 0U, 0, s_incoming_conn);"
            assert call in body
            body = body.replace(call, "(void)s_incoming_conn;")
        if mutation == "missing-radio-disconnect" and name == "event_work_handler":
            call = "(void)bt_conn_disconnect(event.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);"
            assert call in body
            body = body.replace(call, "(void)event.conn;")
        if mutation == "unfrozen-local" and name == "subevent_data_cb":
            assert "atomic_get(&s_local_data_frozen) != 0" in body
            body = body.replace("atomic_get(&s_local_data_frozen) != 0", "atomic_get(&s_local_data_frozen) < 0")
        if mutation == "unfrozen-peer" and name == "ranging_data_received_cb":
            assert "atomic_get(&s_peer_data_frozen) != 0" in body
            body = body.replace("atomic_get(&s_peer_data_frozen) != 0", "atomic_get(&s_peer_data_frozen) < 0")
        if mutation == "missing-rf-summary" and name == "runtime_adapter_complete":
            assert "runtime_emit_rf_diagnostics(step);" in body
            body = body.replace("runtime_emit_rf_diagnostics(step);", "if (step == NULL) runtime_emit_rf_diagnostics(step);")
        if mutation == "missing-method" and name == "estimate_measurement":
            assert "measurement.outcome = NRF_CS_OUTCOME_SUCCESS;" in body
            body = body.replace("measurement.outcome = NRF_CS_OUTCOME_SUCCESS;",
                                "measurement.provider_quality_flags &= ~0x70U; measurement.outcome = NRF_CS_OUTCOME_SUCCESS;")
        if mutation == "missing-queue-timing" and name == "runtime_adapter_complete":
            assignment = "timing.total_ms = (uint32_t)(k_uptime_get() - (s_attempt_deadline_ms - step->timeout_ms));"
            assert assignment in body
            body = body.replace(assignment, "timing.total_ms = timing.measured_ms + timing.cleanup_ms;")
        if mutation == "stale-timing-context" and name == "runtime_adapter_complete":
            assert "if (context == &s_adapter) {" in body
            body = body.replace("if (context == &s_adapter) {", "if (context == &s_adapter || context == NULL) {")
        if mutation == "disabled-is-enabled" and name == "procedure_enable_complete_cb":
            condition = "params->state == BT_CONN_LE_CS_PROCEDURES_ENABLED"
            assert condition in body
            body = body.replace(condition, "true")
        line = source[:start].count("\n") + 1
        path = parts[part - 1].relative_to(ROOT).as_posix()
        provenance.append({"function": name, "path": path, "line": line,
                           "source_sha256": original_digest,
                           "compiled_sha256": hashlib.sha256(body.encode()).hexdigest(),
                           "intentional_mutation": hashlib.sha256(body.encode()).hexdigest() != original_digest})
        (output / f"{name}.inc").write_text(f'#line {line} "{path}"\n{body}\n', encoding="utf-8")
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    manifest = {"version": 1, "tests": [{
        "name": "nrf_cs_production_handlers",
        "sources": ["nRF54L15/src/nrf_cs_pair_adapter.c", "nRF54L15/src/nrf_cs_measurement_quality.c", "nRF54L15/src/nrf_cs_event_overflow.c", "nRF54L15/src/nrf_cs_cycle_runtime.c", "nRF54L15/src/nrf_cs_cycle_protocol.c", "nRF54L15/src/nrf_cs_scheduler.c", "nRF54L15/src/nrf_upstream_protocol.c", "tests/host/nrf_cs_pair_runtime_handlers_probe.c"],
        "include_dirs": ["nRF54L15/include", output.relative_to(ROOT).as_posix()],
        "libraries": ["m"], "defines": ["PROBE_SDK_RAS"] if sdk_ras else [],
    }]}
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "tools/run_host_tests.py", "--manifest", str(manifest_path),
         "--build-dir", str(output / "build")],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", check=False,
    )
    (output / "run.log").write_text(result.stdout, encoding="utf-8")
    if mutation not in ("production", "sdk-ras"):
        expected = {**{key: "s_callbacks==1U && s_outcome==NRF_CS_OUTCOME_TRANSPORT_ERROR && s_de_calls==1U"
                       for key in ("overflow-error-not-saved", "overflow-error-not-drained", "measurement-commit-error-bypass")},
                    "overflow-error-overwrite-first": "s_last_error==status && !s_overflow_error_pending",
                    "overflow-error-foreign": "!s_overflow_error_pending",
                    "overflow-error-no-wakeup": "s_event_queue_overflow_total==overflows+2U && s_event_submits==submits+2U",
                    **{key: "s_local_steps.len==0U && s_peer_owned.len==0U && s_rf_flags==0 && s_count==0U"
                       for key in ("phase-check-bypass", "phase-local-bypass", "phase-enable-bypass", "phase-peer-bypass", "phase-ready-bypass", "phase-overwritten-bypass")},
                    "payload-counter-unchecked": "s_callbacks==1U && s_outcome==(fault==0U?NRF_CS_OUTCOME_SUCCESS:NRF_CS_OUTCOME_OUTLIER)",
                    "payload-config-unchecked": "s_callbacks==1U && s_outcome==(fault==0U?NRF_CS_OUTCOME_SUCCESS:NRF_CS_OUTCOME_OUTLIER)",
                    "local-config-unchecked": "s_count==0U && s_local_steps.len==0U && s_rf_flags==0",
                    "enable-config-unchecked": "s_rf_flags==0 && s_rf_tx_power==NRF_CS_RF_UNAVAILABLE",
                    "missing-rf-summary": "s_rf_signal_events", "missing-method": "s_provider_flags",
                    "missing-queue-timing": "s_trace_result_timing.measured_ms==100",
                    "stale-timing-context": "s_trace_result_timing.cleanup_ms==0",
                    "disabled-is-enabled": "s_reflector_procedure_active==0",
                    "unfrozen-local": "s_local_steps.len==2",
                    "unfrozen-peer": "s_peer_owned.len==2",
                    "missing-radio-stop": "s_count==1 && s_disconnect_calls==0",
                    "missing-radio-disconnect": "s_disconnect_calls==1",
                    "missing-cancel": "s_cleanup_calls==1",
                    "foreign-cancel": "s_cleanup_calls==0",
                    "queued-cancel-starts": "s_callbacks==2",
                    "cancel-not-submitted": "s_event_submits==submits+1",
                    "forget-live-disconnect": "s_outgoing_conn==&s_connection",
                    "cleanup-no-fallback": "s_callbacks==1 && s_outcome==NRF_CS_OUTCOME_SUCCESS",
                    "no-disconnect-retry": "s_disconnect_calls==2",
                    "normal-disconnect-releases": "s_release_notifications==",
                    "lost-disconnect-no-deadline": "s_callbacks==1 && !nrf_cs_pair_adapter_is_active",
                    "lost-disconnect-no-worker": "s_callbacks==1 && !nrf_cs_pair_adapter_is_active",
                    "lost-incoming-no-mailbox": "Assertion failed: nrf_cs_pair_runtime_zephyr_is_progress_required()",
                    "lost-incoming-no-release": "s_release_notifications==(i%3U==0U?1U:0U)",
                    "lost-incoming-not-cleared": "!nrf_cs_pair_runtime_zephyr_is_busy() &&",
                    "lost-incoming-new-owner": "s_incoming_conn==NULL && premature.refs==1",
                    "forget-advertising-stop": "result==faults[fault] && s_advertising",
                    "scan-after-stop-error": "result==faults[fault] && s_advertising",
                    "sdk-disconnect-no-free": "slot.conn==NULL && s_connection.refs==0",
                    "sdk-alloc-no-reset": "!slot.on_demand_rd.data_get_in_progress && slot.counter_in_progress==0U",
                    "sdk-ignore-get-error": "s_count==1U && rreq.cp.state==BT_RAS_RREQ_CP_STATE_NONE",
                    "sdk-early-idle": "s_reentrant_get==-EBUSY",
                    "sdk-rx-reset": "s_peer_owned.len==8U && s_peer_owned.data[0]==8U",
                    "warm-reset-active": "!runtime_reset_procedure(next)",
                    "warm-reset-unfrozen": "!runtime_reset_procedure(next)",
                    "warm-reset-same-counter": "!runtime_reset_procedure(next)",
                    "procedure-loses-config": "s_cs_config.channel_map[0]==0xa5 && s_realtime_ras",
                    "cold-reset-expects-zero": "Assertion failed: s_rf_tx_power==NRF_CS_RF_UNAVAILABLE && !s_local_data_frozen && !s_peer_data_frozen",
                    "local-counter-unchecked": "s_count==0 && s_local_steps.len==0",
                    "peer-counter-unchecked": "s_count==0 && s_local_steps.len==0",
                    "ready-counter-unchecked": "s_count==0 && s_local_steps.len==0"}[mutation]
        assert result.returncode != 0 and "(run)" in result.stdout and expected in result.stdout, result.stdout
    else:
        assert result.returncode == 0, f"Production handler probe failed; artifacts: {output}\n{result.stdout}"
