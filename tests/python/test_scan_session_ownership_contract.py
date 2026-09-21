"""Source contracts for session-owned 3D scans (MAX-167)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_scan_paths_are_built_under_the_session_directory_not_the_object():
    source = read("components/web_server/web_scan_store.c")
    assert '"%s/%s/sessions/%s/%s"' in source
    # The old object-direct layout must actually be gone, not just
    # superseded by a second path builder left behind.
    assert '"%s/%s/%s",\n        STORAGE_MANAGER_OBJECTS_PATH,\n        object_id,\n        SCAN_FILE_NAME' not in source


def test_session_is_validated_against_real_recorded_sessions():
    """A scan may only attach to a session that actually exists for the
    object -- this is also the request's security boundary against a
    malformed/malicious session_id, so it must run before any path is used
    for I/O, not merely be present somewhere in the file."""
    source = read("components/web_server/web_scan_store.c")
    assert "seismic_recorder_list_sessions(" in source
    assert "web_scan_verify_session(" in source

    for action in ("web_scan_save", "web_scan_load", "web_scan_delete"):
        start = source.index(f"static esp_err_t {action}(")
        body_end = source.index("\n}\n", start)
        body = source[start:body_end]
        verify_at = body.index("web_scan_verify_session(")
        paths_at = body.index("web_scan_paths(")
        assert verify_at < paths_at, f"{action} must verify the session before building its path"


def test_scan_query_requires_a_session_id():
    source = read("components/web_server/web_scan_store.c")
    handler = source[source.index("esp_err_t web_scan_store_try_handle("):]
    assert '"session_id"' in handler


def test_legacy_object_level_scans_are_migrated_to_the_newest_session_not_deleted_or_guessed():
    """User decision (2026-09-21): attach each legacy object-level scan to
    that object's newest recorded session. Never silently delete a legacy
    scan, and never invent a session for an object that has none."""
    migration = read("components/object_recovery/object_recovery_legacy_scan.c")
    orchestrator = read("components/object_recovery/object_recovery.c")

    assert "seismic_recorder_list_sessions(" in migration
    assert "newest_session_id" in migration
    assert "remove(" not in migration
    assert "object_recovery_migrate_legacy_scan(" in orchestrator


def test_webui_scan_transport_is_session_scoped():
    device_api = read("webui/src/api/deviceApi.ts")
    app = read("webui/src/App.tsx")
    objects_feature = read("webui/src/features/objects/ObjectsFeature.tsx")

    assert "session_id: sessionId" in device_api
    assert "sessionId: string | null" in app
    assert "onOpenScanner: (objectId: string, sessionId: string) => void" in objects_feature
