import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _body_end(source: str, open_brace: int) -> int:
    depth = 0
    state = "code"
    index = open_brace
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError("unclosed C function body")


def _function_bodies(source: str) -> dict[str, list[str]]:
    functions: dict[str, list[str]] = {}
    pattern = re.compile(r"\bstatic\s+void\s+(test_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*\{")
    for match in pattern.finditer(source):
        start = match.end() - 1
        end = _body_end(source, start)
        functions.setdefault(match.group(1), []).append(source[start + 1 : end])
    return functions


def _reachable_tests(source: str) -> set[str]:
    functions = _function_bodies(source)
    main = re.search(r"\bint\s+main\s*\([^)]*\)\s*\{", source)
    assert main, "host test has no main()"
    end = _body_end(source, main.end() - 1)
    pending = set(re.findall(r"\b(test_[A-Za-z0-9_]+)\s*\(", source[main.end() : end]))
    reachable: set[str] = set()
    while pending:
        name = pending.pop()
        if name in reachable or name not in functions:
            continue
        reachable.add(name)
        for body in functions[name]:
            pending.update(re.findall(r"\b(test_[A-Za-z0-9_]+)\s*\(", body))
    return reachable


def _manifest_entry(path: str, name: str) -> dict:
    manifest = json.loads((ROOT / path).read_text(encoding="utf-8"))
    entries = [entry for entry in manifest["tests"] if entry.get("name") == name]
    assert len(entries) == 1, f"expected one {name} entry in {path}"
    return entries[0]


def _assert_registered(manifest_path: str, name: str, source_path: str) -> None:
    entry = _manifest_entry(manifest_path, name)
    sources = {str(source).replace("\\", "/") for source in entry.get("sources", [])}
    assert source_path in sources, f"{source_path} is not registered in {manifest_path}"


def _assert_tests_reachable(source_path: str, names: tuple[str, ...]) -> None:
    source = (ROOT / source_path).read_text(encoding="utf-8")
    declared = set(_function_bodies(source))
    reachable = _reachable_tests(source)
    for name in names:
        function = f"test_{name}"
        assert function in declared, f"missing {function} in {source_path}"
        assert function in reachable, f"{function} is not reachable from main()"


def test_max484_host_cases_are_reachable_from_main():
    cases = (
        (
            "components/object_recovery/tests/object_recovery_fs_contract_test.c",
            (
                "scan_deletes_only_after_all_metadata_candidates_fail",
                "active_and_leased_objects_are_never_removed",
                "unrecoverable_delete_emits_one_complete_record",
                "boot_delete_records_boot_timing",
                "delete_is_skipped_when_scope_count_fails",
            ),
        ),
        (
            "components/seismic_recorder/tests/seismic_recorder_session_geometry_test.c",
            ("summary_reader_accepts_v1_v2_and_rejects_unsupported_versions",),
        ),
        (
            "components/object_store/tests/object_store_fs_contract_test.c",
            ("delete_scope_counts_zero_one_and_many",),
        ),
        (
            "tests/host/app_startup_reset_reason_test.c",
            ("reset_reason_mapping", "reset_reason_is_logged_and_available_to_status"),
        ),
        (
            "tests/host/web_status_json_builder_test.c",
            ("reset_reason_is_serialized",),
        ),
    )
    for source_path, names in cases:
        _assert_tests_reachable(source_path, names)


def test_max484_host_cases_are_registered_in_root_and_fragments():
    registrations = (
        ("tests/host/manifest.json", "object_recovery", "components/object_recovery/tests/object_recovery_fs_contract_test.c"),
        ("tests/host/manifest.d/coverage-object-recovery.json", "object_recovery", "components/object_recovery/tests/object_recovery_fs_contract_test.c"),
        ("tests/host/manifest.json", "seismic_recorder_session_geometry", "components/seismic_recorder/tests/seismic_recorder_session_geometry_test.c"),
        ("tests/host/manifest.d/max444-recording-gate.json", "seismic_recorder_session_geometry", "components/seismic_recorder/tests/seismic_recorder_session_geometry_test.c"),
        ("tests/host/manifest.json", "object_store_fs", "components/object_store/tests/object_store_fs_contract_test.c"),
        ("tests/host/manifest.d/max459-delete-preview.json", "max459_object_store_delete_scope", "components/object_store/tests/object_store_fs_contract_test.c"),
        ("tests/host/manifest.json", "app_startup_reset_reason", "tests/host/app_startup_reset_reason_test.c"),
        ("tests/host/manifest.d/max484-reset-reason.json", "app_startup_reset_reason", "tests/host/app_startup_reset_reason_test.c"),
        ("tests/host/manifest.json", "web_status_json_builder", "tests/host/web_status_json_builder_test.c"),
        ("tests/host/manifest.d/coverage-web-status.json", "web_status_json_builder", "tests/host/web_status_json_builder_test.c"),
    )
    for manifest_path, target, source_path in registrations:
        _assert_registered(manifest_path, target, source_path)
