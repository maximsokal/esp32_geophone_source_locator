"""Execute exact production CS handler bodies with fake external APIs/clock.

This is a host seam test, not execution of Zephyr or the Nordic radio stack.
Source bodies are extracted unchanged; their digests remain beside the result.
"""
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("mutation", ["production", "missing-rf-summary", "missing-method", "missing-queue-timing", "stale-timing-context", "disabled-is-enabled", "unfrozen-local", "unfrozen-peer", "missing-radio-stop", "missing-radio-disconnect", "missing-cancel", "foreign-cancel", "queued-cancel-starts", "cancel-not-submitted", "forget-live-disconnect", "cleanup-no-fallback", "no-disconnect-retry", "normal-disconnect-releases"])
def test_production_cs_handlers_enforce_deadline_and_reject_foreign_steps(mutation):
    build_root = ROOT / ".test-build"
    build_root.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="cs-handler-proof-", dir=build_root))
    parts = [ROOT / f"nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_{i}.inc" for i in (1, 2, 3)]
    parts.append(ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_connections.inc")
    sources = [p.read_text(encoding="utf-8") for p in parts]
    adapter_ops = sources[1].split("static const nrf_cs_pair_adapter_ops_t s_adapter_ops = {", 1)[1].split("};", 1)[0]
    assert ".now_ms = op_now_ms," in adapter_ops  # The target adapter must actually enable its clock.
    provenance = []
    types = sources[0][sources[0].index("typedef enum {"):sources[0].index("K_MSGQ_DEFINE(")]
    (output / "production_types.inc").write_text(types, encoding="utf-8")
    for part, name in (
        (1, "runtime_reset_ranging"), (1, "ranging_data_received_cb"),
        (1, "runtime_take_cancel"), (1, "nrf_cs_pair_runtime_zephyr_cancel"),
        (4, "disconnected_cb"),
        (2, "runtime_emit_rf_diagnostics"), (2, "report_ranging_header"),
        (2, "accumulate_mean"), (2, "collect_pbr_step"),
        (2, "collect_rtt_step"), (2, "report_step_data"),
        (2, "current_tone_stats"), (2, "good_tone_channel_count"), (2, "estimate_measurement"),
        (2, "op_now_ms"), (2, "runtime_adapter_complete"), (2, "timeout_work_handler"),
        (2, "op_cleanup"),
        (2, "cleanup_work_handler"), (2, "maybe_complete_measurement"),
        (3, "event_work_handler"), (3, "execute_work_handler"),
        (3, "nrf_cs_pair_runtime_zephyr_execute"),
        (3, "nrf_cs_pair_runtime_zephyr_set_radio_enabled"),
        (3, "procedure_enable_complete_cb"), (3, "subevent_data_cb"),
        (3, "nrf_cs_pair_runtime_zephyr_is_progress_required"),
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
        "sources": ["nRF54L15/src/nrf_cs_pair_adapter.c", "nRF54L15/src/nrf_cs_measurement_quality.c", "tests/host/nrf_cs_pair_runtime_handlers_probe.c"],
        "include_dirs": ["nRF54L15/include", output.relative_to(ROOT).as_posix()],
        "libraries": ["m"],
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
    if mutation != "production":
        expected = {"missing-rf-summary": "s_rf_signal_events", "missing-method": "s_provider_flags",
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
                    "normal-disconnect-releases": "s_release_notifications=="}[mutation]
        assert result.returncode != 0 and "(run)" in result.stdout and expected in result.stdout, result.stdout
    else:
        assert result.returncode == 0, f"Production handler probe failed; artifacts: {output}\n{result.stdout}"
