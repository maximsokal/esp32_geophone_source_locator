"""Static MAX-72 contracts for measurement object authority and mutation admission."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_measurement_service_owns_one_shared_object_mutation_policy():
    header = read("components/measurement_service/include/measurement_service.h")
    source = read("components/measurement_service/measurement_service.c")
    body = function_body(
        source,
        "bool measurement_service_snapshot_allows_object_mutation"
    )

    assert "measurement_service_snapshot_allows_object_mutation" in header
    assert "snapshot == NULL || snapshot->busy" in body
    assert "MEASUREMENT_SERVICE_LIVE" in body
    assert "MEASUREMENT_SERVICE_REVIEW" in body
    assert "MEASUREMENT_SERVICE_ERROR" in body
    assert "MEASUREMENT_SERVICE_STARTING" not in body
    assert "MEASUREMENT_SERVICE_RECORDING" not in body
    assert "MEASUREMENT_SERVICE_STOPPING" not in body
    assert "MEASUREMENT_SERVICE_DELETING" not in body


def test_web_and_tft_objects_use_the_same_admission_helper():
    web = read("components/web_server/web_api_control.c")
    ui = read("components/ui/ui_objects_input.c")

    web_gate = function_body(web, "static bool web_control_object_storage_is_idle")
    ui_gate = function_body(
        ui,
        "static bool ui_objects_measurement_mutation_allowed_locked"
    )

    assert "measurement_service_snapshot_allows_object_mutation" in web_gate
    assert "MEASUREMENT_SERVICE_" not in web_gate
    assert "measurement_service_snapshot_allows_object_mutation" in ui_gate
    assert "MEASUREMENT OPERATION IN PROGRESS" in ui_gate

    assert ui.count("ui_objects_measurement_mutation_allowed_locked()") == 3
    assert "object_service_create_async" in ui
    assert "object_service_set_active_async" in ui
    assert "object_service_delete_async" in ui


def test_session_owner_is_not_reprojected_from_current_active():
    source = read("components/measurement_service/measurement_service.c")
    snapshot_body = function_body(source, "esp_err_t measurement_service_get_snapshot")
    start_body = function_body(source, "static void measurement_service_process_start")
    keep_body = function_body(source, "static void measurement_service_process_keep")
    delete_body = function_body(source, "static void measurement_service_process_delete")

    assert "session_owner_present" in snapshot_body
    assert "project_current_active" in snapshot_body
    assert "MEASUREMENT_SERVICE_LIVE" in snapshot_body
    assert "MEASUREMENT_SERVICE_ERROR && !session_owner_present" in snapshot_body
    assert "if (project_current_active)" in snapshot_body

    assert "measurement_service_clear_session_identity_locked();" in start_body
    assert "measurement_service_clear_session_identity_locked();" in keep_body
    assert "measurement_service_clear_session_identity_locked();" in delete_body


def test_measurement_tft_renders_session_owner_independently_of_global_active():
    ui = read("components/ui/ui_measurements.c")
    refresh = function_body(ui, "void ui_measurements_refresh_locked")

    assert "idle_object_context" in refresh
    assert "displayed_object_present" in refresh
    assert "measurement.object_id[0] != '\\0'" in refresh
    assert "? measurement.active_object_present" in refresh
    assert "RESOLVING OBJECT..." in refresh
