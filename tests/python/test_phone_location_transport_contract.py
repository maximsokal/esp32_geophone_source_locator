"""Contracts for the phone-GPS request and status round trip."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


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


def test_phone_gps_request_is_validated_and_persisted_as_phone_source() -> None:
    source = read("components/web_server/web_api_control.c")
    process = function_body(source, "static esp_err_t web_control_process")
    location = process[process.index('strcmp(action, "set_object_location")'):]

    for field in ("latitude", "longitude", "accuracy_m", "captured_at_ms"):
        assert f'web_control_number(root, "{field}"' in location
    assert "captured < 0.0" in location
    assert 'snprintf(location.source, sizeof(location.source), "PHONE_GPS")' in location
    assert "object_store_set_location(object_id, &location)" in location
    assert 'strcmp(action, "set_object_location") == 0' in source


def test_phone_gps_status_contains_the_complete_persisted_location() -> None:
    source = read("components/web_server/web_status_json_objects.c")
    location = function_body(source, "static bool web_status_json_add_location")

    for field in (
        "valid",
        "latitude",
        "longitude",
        "accuracy_m",
        "altitude_valid",
        "altitude_m",
        "altitude_accuracy_m",
        "captured_at_ms",
        "source",
    ):
        assert f'"{field}"' in location


def test_webui_phone_gps_flow_posts_to_control_and_reprojects_confirmation() -> None:
    app = read("webui/src/DeviceConsoleApp.tsx")
    api = read("webui/src/api/deviceApi.ts")
    mutation = read("webui/src/features/objects/objectMutation.ts")

    assert "capturePhoneLocation()" in app
    assert "runControl('set_object_location'" in app
    assert "jsonRequest<ControlResult>('/api/control'" in api
    assert "source: 'PHONE_GPS'" in mutation
