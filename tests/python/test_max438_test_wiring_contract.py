import json
import os
import re
from pathlib import Path


ROOT = Path(os.environ.get("MAX438_TEST_ROOT", Path(__file__).resolve().parents[2]))
POLICY_TEST = Path("components/nrf_topology_service/tests/nrf_node_link_status_policy_test.c")
UI_TEST = Path("components/ui/tests/ui_node_link_status_test.c")
TRANSPORT_TEST = Path("tests/host/nrf_sensor_transport_test.c")
WEB_JSON_TEST = Path("tests/host/web_status_json_builder_test.c")
STATUS_KEYS = {
    "measurement.link_not_connected": "not connected",
    "measurement.link_connected": "connected",
    "measurement.link_ready": "ready",
    "measurement.link_measuring": "measuring",
    "measurement.link_legend": "LINK",
}


def _function_body(source: str, name: str) -> str:
    match = re.search(rf"\b(?:static\s+)?(?:void|int)\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    assert match, f"missing function {name}"
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.end() : index]
    raise AssertionError(f"unclosed function {name}")


def _assert_test_is_called_from_main(path: Path, test_name: str) -> None:
    source = (ROOT / path).read_text(encoding="utf-8")
    main = _function_body(source, "main")
    assert re.search(rf"\b{re.escape(test_name)}\s*\(\s*\)", main), (
        f"{test_name} is not called from main() in {path}"
    )


def _assert_target(manifest_path: Path, target: str, source_path: Path) -> dict:
    manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
    entries = [item for item in manifest["tests"] if item.get("name") == target]
    assert len(entries) == 1, f"expected one {target} target in {manifest_path}"
    sources = {str(item).replace("\\", "/") for item in entries[0].get("sources", [])}
    assert source_path.as_posix() in sources, f"{source_path} is not registered in {manifest_path}"
    return entries[0]


def test_link_policy_cases_are_reachable_and_registered():
    test_names = (
        "test_status_not_connected_when_node_absent_unassigned_or_error",
        "test_status_connected_when_fresh_status_not_ready",
        "test_status_ready_when_fresh_ready_ok_assigned_registered",
        "test_status_measuring_when_ready_and_recording",
        "test_stale_limit_includes_exact_boundary",
        "test_busy_window_keeps_last_good_status_until_limit",
        "test_identity_mismatch_is_not_connected",
        "test_refresh_and_stale_tolerances_are_pinned",
    )
    for name in test_names:
        _assert_test_is_called_from_main(POLICY_TEST, name)
    entry = _assert_target(
        Path("tests/host/manifest.json"),
        "nrf_node_link_status_policy",
        POLICY_TEST,
    )
    sources = {str(item).replace("\\", "/") for item in entry["sources"]}
    assert "components/nrf_topology_service/nrf_node_link_status_policy.c" in sources


def test_ui_status_cases_are_reachable_and_registered():
    test_names = (
        "test_status_widget_changes_only_when_status_changes",
        "test_status_colors_and_text_keys",
        "test_channel_to_node_map_and_g4_unmapped",
    )
    for name in test_names:
        _assert_test_is_called_from_main(UI_TEST, name)
    entry = _assert_target(Path("tests/host/manifest.json"), "ui_node_link_status", UI_TEST)
    sources = {str(item).replace("\\", "/") for item in entry["sources"]}
    assert "components/ui/ui_node_link_status.c" in sources


def test_transport_cache_cases_are_reachable_and_registered():
    for name in (
        "test_status_cache_retains_lifecycle_result_flags",
        "test_fresh_booting_status_is_retained_without_reconcile",
        "test_status_cache_survives_busy_topology_refresh",
    ):
        _assert_test_is_called_from_main(TRANSPORT_TEST, name)
    for manifest_path in (
        Path("tests/host/manifest.json"),
        Path("tests/host/manifest.d/coverage-tca.json"),
        Path("tests/host/manifest.d/coverage-nrf.json"),
    ):
        _assert_target(manifest_path, "nrf_sensor_transport", TRANSPORT_TEST)


def test_web_json_link_status_case_is_reachable_and_registered():
    _assert_test_is_called_from_main(WEB_JSON_TEST, "test_node_link_status_is_added_to_pods")
    for manifest_path in (
        Path("tests/host/manifest.json"),
        Path("tests/host/manifest.d/coverage-web-status.json"),
    ):
        entry = _assert_target(manifest_path, "web_status_json_builder", WEB_JSON_TEST)
        sources = {str(item).replace("\\", "/") for item in entry["sources"]}
        assert "components/nrf_topology_service/nrf_node_link_status_policy.c" in sources


def test_status_translations_and_webui_shape_are_pinned():
    catalog = json.loads((ROOT / "i18n/tft.json").read_text(encoding="utf-8"))["entries"]
    for key, english in STATUS_KEYS.items():
        assert key in catalog, f"missing TFT translation {key}"
        entry = catalog[key]
        assert entry["en"] == english
        assert entry["uk"].strip()
        assert len(entry["en"]) <= entry["max_len"]
        assert len(entry["uk"]) <= entry["max_len"]

    device_types = (ROOT / "webui/src/domain/device.ts").read_text(encoding="utf-8")
    schema = (ROOT / "webui/src/api/deviceStatusSchema.ts").read_text(encoding="utf-8")
    assert "node_link_status?: NodeLinkStatus;" in device_types
    assert "optionalString(pod, 'node_link_status', path);" in schema


def test_measurements_reads_cached_node_status_and_updates_widgets_on_change():
    source = (ROOT / "components/ui/ui_measurements.c").read_text(encoding="utf-8")
    assert "nrf_topology_service_get_node_status_snapshot" in source
    assert "ui_node_link_status_changed" in source
    assert "measurement_link_status_valid[channel]" in source
    assert "lv_chart_set_series_color(chart, minimum, color);" in source
    assert "ui_measurements_label_set_if_changed(" in source
    policy = (ROOT / "components/nrf_topology_service/nrf_topology_service.c").read_text(encoding="utf-8")
    refresh = _function_body(policy, "service_process_sensor_mask")
    assert "snapshot->bus_busy || snapshot->stale" in refresh
    assert "service_requeue_inflight_mask(mask);" in refresh

