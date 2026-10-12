import json
from pathlib import Path

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
