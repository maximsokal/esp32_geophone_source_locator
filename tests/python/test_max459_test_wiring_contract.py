import json
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_max459_tests_are_wired_and_localized():
    manifest_path = ROOT / "tests/host/manifest.d/max459-delete-preview.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    names = {entry["name"] for entry in manifest["tests"]}
    assert {"max459_object_store_delete_scope", "max459_ui_objects_delete_model"} <= names
    assert "test_delete_scope_counts_zero_one_and_many();" in read("components/object_store/tests/object_store_fs_contract_test.c")
    assert "test_zero_counts();" in read("components/ui/tests/ui_objects_delete_model_test.c")
    assert "test_sensor_frame_fallback_with_method_estimate_marker_limits();" in read("components/ui/tests/ui_preview3d_test.c")
    assert "ui_objects_delete_model.c" in read("components/ui/CMakeLists.txt")

    web_entries = json.loads(read("i18n/web.json"))["entries"]
    for key in ("objects.delete_confirm_scope", "objects.delete_confirm_scope_unavailable",
                "objects.controls_locked_recording", "overlay.method_estimates_one_source"):
        assert key in web_entries and web_entries[key]["en"] and web_entries[key]["uk"]
    tft_entries = json.loads(read("i18n/tft.json"))["entries"]
    for key in ("objects.delete_scope.sessions", "objects.delete_scope.artifacts", "objects.delete_scope.runs",
                "objects.delete_scope.unavailable", "scan.sensor_estimates_frame", "scan.method_estimates"):
        assert key in tft_entries and tft_entries[key]["en"] and tft_entries[key]["uk"]


def test_workflow_screen_open_does_not_start_measurement():
    source = read("components/ui/ui_workflow.c")
    start = source.index("void ui_workflow_screen_open_locked(void)")
    end = source.index("\nvoid ", start + len("void ui_workflow_screen_open_locked(void)"))
    body = source[start:end]
    assert "WORKFLOW_COMMAND_START_MEASUREMENT" not in body
    route = read("tests/host/ui_home_navigation_test.c")
    assert "s_workflow_snapshot.step == WORKFLOW_STEP_OBJECT" in route
    assert "s_acquisition_start_calls == 0U" in route


def test_delete_scope_is_serialized_and_controls_read_it():
    server = read("components/web_server/web_status_json_objects.c")
    assert "object_store_get_delete_scope(session_cache->object_id, &session_cache->delete_scope)" in server
    for field in ("available", "sessions", "artifacts", "localization_runs"):
        assert f'"{field}"' in server
    web = read("webui/src/features/objects/ObjectsFeature.tsx")
    assert "objects.delete_confirm_scope" in web
    assert "objects.controls_locked_recording" in web
    assert "disabled={objectMutationDisabled}" in web


def montserrat_14_pixel_width(text: str) -> float:
    font = read("components/ui/fonts/ui_font_montserrat_14.c")
    glyph_match = re.search(
        r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};",
        font,
        re.S,
    )
    assert glyph_match is not None
    advances = [int(value) for value in re.findall(r"\.adv_w\s*=\s*(\d+)", glyph_match.group(1))]
    assert advances

    unicode_lists = {
        name: [int(value, 0) for value in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]
        for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};",
            font,
            re.S,
        )
    }
    cmap_match = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps\[\] =\s*\{(.*?)\n\};",
        font,
        re.S,
    )
    assert cmap_match is not None
    cmaps = []
    for block in re.findall(r"\{(.*?)\}", cmap_match.group(1), re.S):
        field = lambda name: re.search(rf"\.{name}\s*=\s*([^,\s]+)", block)
        start = field("range_start")
        length = field("range_length")
        glyph_start = field("glyph_id_start")
        unicode_list = field("unicode_list")
        if start is None or length is None or glyph_start is None:
            continue
        sparse = None if unicode_list is None or unicode_list.group(1) == "NULL" else unicode_lists[unicode_list.group(1)]
        cmaps.append((int(start.group(1), 0), int(length.group(1), 0), int(glyph_start.group(1), 0), sparse))

    width = 0.0
    for character in text:
        codepoint = ord(character)
        glyph_id = None
        for start, length, glyph_start, sparse in cmaps:
            offset = codepoint - start
            if offset < 0 or offset >= length:
                continue
            if sparse is None:
                glyph_id = glyph_start + offset
            elif offset in sparse:
                glyph_id = glyph_start + sparse.index(offset)
            if glyph_id is not None:
                break
        assert glyph_id is not None, f"font has no glyph for U+{codepoint:04X} ({character!r})"
        assert glyph_id < len(advances)
        width += advances[glyph_id] / 16.0
    return width


def test_max460_tft_strings_fit_codepoint_and_font_pixel_budgets():
    entries = json.loads(read("i18n/tft.json"))["entries"]
    keys = (
        "tft.workflow.title.gps_choice",
        "tft.workflow.title.gps_pending",
        "tft.workflow.title.layout_ready",
        "tft.workflow.title.scan_choice",
        "tft.workflow.title.scanning",
        "tft.workflow.gps.waiting",
        "tft.workflow.layout.ready",
        "tft.workflow.scan.phone",
        "tft.workflow.action.placement_confirmed",
        "tft.workflow.action.skip",
    )
    for key in keys:
        entry = entries[key]
        for locale in ("en", "uk"):
            text = entry[locale]
            assert len(text) <= entry["max_len"], (key, locale, len(text), entry["max_len"])
            assert montserrat_14_pixel_width(text) <= 304.0, (key, locale, text)


def test_max460_workflow_tests_are_in_main_and_manifest_targets():
    core = read("tests/host/workflow_core_test.c")
    service = read("tests/host/workflow_service_test.c")
    wfa = read("tests/host/workflow_service_wfa_test.c")
    ui = read("components/ui/tests/ui_workflow_model_test.c")
    api = read("tests/host/web_api_control_http_test.c")
    web_status_storage = read("components/web_server/tests/web_status_recording_gate_storage_test.c")
    web_workflow = read("webui/tests/console/workflow.test.tsx")
    assert "test_tft_webui_allowed_actions_generation_and_reconnect_parity" in web_workflow
    scanner_workflow = read("webui/tests/console/deviceConsoleAppScannerBoundary.test.tsx")
    recorder_fs = read("components/seismic_recorder/tests/seismic_recorder_fs_contract_test.c")
    for name in (
        "test_phone_gps_validates_and_persists_only_accepted_fixes();",
        "test_gps_failure_back_skip_and_late_callback_generation();",
        "test_gps_skip_keeps_existing_location_and_marks_run_skipped();",
        "test_scan_choice_requires_explicit_skip_before_recording();",
        "test_new_workflow_steps_wait_for_explicit_commands();",
        "test_staged_scan_remains_visible_across_session_creation();",
    ):
        assert name in service
    assert "test_all_steps();" in ui
    assert "test_max460_tft_action_set_matches_core_for_guided_steps();" in ui
    assert "test_exhaustive_transition_table();" in core
    assert "test_calib_result_requires_explicit_place_sensors();" in core
    assert "test_no_step_advances_without_a_command_fake_clock();" in service
    assert "test_back_clears_downstream_workflow_choices();" in core
    assert "test_ranging_success_and_confirm_preserve_frozen_result();" in wfa
    assert "test_max460_layout_success_failure_reason_retry_and_back();" in wfa
    assert "test_placement_sample_count_and_calculate_guard();" in wfa
    assert "test_placement_uses_inclusive_three_second_window();" in wfa
    assert "test_layout_rechecks_placement_after_gps_choice();" in wfa
    assert "test_max460_guided_step_content_and_placement_gate();" in ui
    assert "test_workflow_gps_accept_parses_fix_and_optional_altitude();" in api
    assert "test_workflow_gps_accept_rejects_malformed_payload_before_dispatch();" in api
    assert "test_workflow_json_serializes_gps_layout_and_scan_status();" in web_status_storage
    assert "test_workflow_scan_promotes_into_recording_session_or_stays_staged();" in recorder_fs
    for name in (
        "requests a fresh phone fix, then reports permission denial",
        "accepts a fresh fix only through GPS_ACCEPT",
        "sends a phone GPS fix with GPS_ACCEPT",
    ):
        assert name in web_workflow
    assert "resumes a workflow step from the device snapshot after a browser reload" in scanner_workflow
    assert "returns a staged workflow scan to the workflow" in scanner_workflow
    assert "marks the staged scan complete and returns to RECORD_READY after upload" in scanner_workflow

    manifests = {
        "coverage-wf1.json": {"workflow_core", "workflow_service", "workflow_control_http", "workflow_status_json"},
        "coverage-wfA.json": {"workflow_service_wfa", "workflow_json_wfa"},
        "coverage-web-status.json": {"web_status_recording_gate_storage"},
        "coverage-wfT.json": {"ui_workflow_model"},
        "coverage-wf6.json": {"wf6_seismic_recorder_staging"},
        "coverage-299c.json": {"seismic_recorder_fs_v2_writer"},
        "max449-measurements-workflow.json": {"max449_n4_low_remote_workflow", "max449_n5_workflow_gate_strict", "max449_n7_workflow_preview_regression"},
    }
    for filename, required in manifests.items():
        manifest = json.loads(read(f"tests/host/manifest.d/{filename}"))
        names = {entry["name"] for entry in manifest["tests"]}
        assert required <= names

    root_manifest = json.loads(read("tests/host/manifest.json"))
    parity_target = next(entry for entry in root_manifest["tests"] if entry["name"] == "max460_workflow_action_parity")
    assert "tests/host/web_status_recording_gate_host_test.c" in parity_target["sources"]
    assert "components/workflow_service/workflow_core.c" in parity_target["sources"]
    assert "MAX460_WORKFLOW_ACTION_PARITY_ONLY=1" in parity_target["defines"]
    ui_manifest = json.loads(read("tests/host/manifest.d/coverage-wfT.json"))
    ui_target = next(entry for entry in ui_manifest["tests"] if entry["name"] == "ui_workflow_model")
    assert "components/ui/ui_workflow_model.c" in ui_target["sources"]
    recorder_target = next(entry for entry in root_manifest["tests"] if entry["name"] == "seismic_recorder")
    assert "components/workflow_service/workflow_core.c" in recorder_target["sources"]


def test_max460_c_action_table_matches_shared_webui_fixture(tmp_path: Path):
    build_dir = tmp_path / "host-action-parity"
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/run_host_tests.py"),
            "--manifest",
            "tests/host/manifest.json",
            "--build-dir",
            str(build_dir),
            "--only",
            "max460_workflow_action_parity",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr

    report = json.loads((build_dir / "host-test-report.json").read_text(encoding="utf-8"))
    assert report["results"][0]["status"] == "PASS"
    output = report["results"][0]["stdout"]
    match = re.search(r"^MAX460_ALLOWED_ACTIONS=(\{.*\})$", output, re.MULTILINE)
    assert match is not None, output
    actual = json.loads(match.group(1))
    expected = json.loads(read("webui/tests/fixtures/max460_workflow_allowed_actions.json"))
    assert set(actual) == set(expected)
    assert {step: sorted(actions) for step, actions in actual.items()} == {
        step: sorted(actions) for step, actions in expected.items()
    }
