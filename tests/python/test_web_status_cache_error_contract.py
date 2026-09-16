from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _function(source: str, signature: str, next_signature: str | None = None) -> str:
    start = source.index(signature)
    if next_signature is None:
        return source[start:]
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def test_cache_serializers_return_typed_errors() -> None:
    header = _source("components/web_server/web_status_cache_sections.h")
    objects = _source("components/web_server/web_status_json_objects.c")

    assert "esp_err_t web_status_json_add_objects(cJSON *root);" in header
    assert "esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id);" in header

    object_fn = _function(
        objects,
        "esp_err_t web_status_json_add_objects(cJSON *root)",
        "static const web_status_session_cache_t *web_status_find_session_cache",
    )
    assert "ESP_ERR_INVALID_STATE" in object_fn
    assert "s_active_payload == NULL" in object_fn
    assert "return ok ? ESP_OK : ESP_ERR_NO_MEM;" in object_fn

    session_fn = _function(
        objects,
        "esp_err_t web_status_json_add_sessions(cJSON *root, const char *object_id)",
    )
    assert "ESP_ERR_INVALID_STATE" in session_fn
    assert "s_active_payload == NULL" in session_fn
    assert "return ESP_ERR_NO_MEM;" in session_fn
    assert "return ESP_OK;" in session_fn


def test_builder_propagates_cache_section_error_without_recheck() -> None:
    builder = _source("components/web_server/web_status_json.c")
    build_fn = _function(
        builder,
        "esp_err_t web_status_json_build_extended(",
        "esp_err_t web_status_json_build(char **json_out)",
    )

    assert "err = web_status_json_add_cache_sections(" in build_fn
    assert "if (err != ESP_OK)" in build_fn
    assert "return err;" in build_fn
    assert "ok = web_status_json_add_objects" not in build_fn
    assert "ok = web_status_json_add_sessions" not in build_fn
    assert "web_status_json_objects_cache_ready();" in build_fn


def test_http_handler_keeps_invalid_state_to_503_contract() -> None:
    api = _source("components/web_server/web_api_status.c")
    assert "err == ESP_ERR_INVALID_STATE" in api
    assert '"503 Service Unavailable"' in api
    assert '{\\"storage_busy\\":true}' in api


def test_focused_host_manifest_is_runnable_by_standard_runner() -> None:
    manifest = _source("tests/host/manifest.d/coverage-web-status.json")
    runner = _source("tools/run_host_tests.py")
    assert '"name": "web_status_cache_sections"' in manifest
    assert "web_status_cache_sections_test.c" in manifest
    assert 'parser.add_argument("--manifest"' in runner
