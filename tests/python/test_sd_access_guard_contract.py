"""Every function that touches the SD card's file system takes the storage operation guard itself.

The SD card and the TFT share SPI2. The guard (storage_manager_operation_begin) serializes whole file operations and
quiesces the display; a file access without it can start an SD transaction while the TFT is sending a frame, and the SPI
driver asserts (spi_hal_setup_trans: spi_ll_get_running_cmd(hw) == 0), a reboot. Board log 2026-11-15 18:04: the
calculation of a 4 kHz recording, an fopen in seismic_recorder_json_samples_consistent.

Helpers named "..._locked", internal helpers and the storage manager's own primitives are called with the guard held by
their caller; they are listed below, one by one, as they were when this test was written. A NEW function that touches the
file system without taking the guard fails this test: take the guard in it, or, if its caller really holds it, add it to
the list with that reason in the commit.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FS_CALLS = re.compile(r"\b(fopen|opendir|stat|fstat|rename|remove|unlink|mkdir|rmdir|truncate)\s*\(")
FUNCTION = re.compile(r"^[A-Za-z_][^\n;{}]*\(([^;{}]|\n)*?\)\s*\n?\{", re.M)

# Helpers whose callers hold the guard (or that run before any other task can touch the card).
GUARD_HELD_BY_CALLER = {
    "components/localization_run_store/localization_run_store.c::localization_run_store_list_locked",
    "components/localization_run_store/localization_run_store.c::localization_run_store_list_picks_locked",
    "components/object_recovery/object_recovery_fs.c::object_recovery_fs_unlink_if_exists",
    "components/object_recovery/object_recovery_fs.c::object_recovery_fs_restore_file",
    "components/object_recovery/object_recovery_legacy_scan.c::object_recovery_file_exists",
    "components/object_recovery/object_recovery_metadata_validate.c::object_recovery_open_file_stat",
    "components/object_recovery/object_recovery_metadata_validate.c::object_recovery_fs_metadata_check",
    "components/object_recovery/object_recovery_scan.c::object_recovery_check_one_with_policy",
    "components/object_store/object_store_fs_ids.c::object_store_fs_entry_is_directory",
    "components/object_store/object_store_fs_ids.c::object_store_fs_entry_is_committed",
    "components/object_store/object_store_fs_ids.c::object_store_fs_next_id",
    "components/object_store/object_store_fs_list.c::object_store_fs_list_entry_is_directory",
    "components/object_store/object_store_fs_list.c::object_store_fs_list",
    "components/object_store/object_store_fs_list.c::object_store_fs_count_delete_scope_directory",
    "components/object_store/object_store_fs_list.c::object_store_fs_get_delete_scope",
    "components/sd_card/sd_card_diagnostics.c::sd_card_diagnostics_cleanup",
    "components/sd_card/sd_card_diagnostics.c::sd_card_probe_log_read_open_diagnostics",
    "components/sd_card/sd_card_diagnostics.c::sd_card_write_readback_probe",
    "components/sd_card/sd_card_diagnostics_io.c::sd_card_diagnostics_write_file",
    "components/sd_card/sd_card_diagnostics_io.c::sd_card_diagnostics_read_file",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_session_metadata_state",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_next_session_id",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_artifact_present",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_next_scan_id",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_write_scan_ref",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_promote_scan",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_restore_staged_artifacts",
    "components/seismic_recorder/seismic_recorder_fs.c::seismic_recorder_fs_restore_staged_artifacts",
    "components/seismic_recorder/seismic_recorder_fs_list.c::seismic_recorder_list_metadata_state",
    "components/seismic_recorder/seismic_recorder_list_responsive.c::seismic_recorder_responsive_metadata_state",
    "components/seismic_recorder/seismic_recorder_lod_store.c::seismic_recorder_lod_store_invalidate_locked",
    "components/storage_manager/storage_manager.c::storage_manager_init",
    "components/storage_manager/storage_manager_directory.c::storage_manager_ensure_directory_locked",
    "components/storage_manager/storage_manager_text_helpers.c::storage_manager_recover_backup_locked",
    "components/storage_manager/storage_manager_text_write.c::storage_manager_write_temp_file",
    "components/storage_manager/storage_manager_tree.c::storage_manager_remove_tree_locked",
    "components/web_server/web_scan_store.c::web_scan_file_exists",
    "components/web_server/web_scan_store.c::web_scan_recover_file_locked",
    "components/web_server/web_scan_store.c::web_scan_commit_temp_locked",
    "components/web_server/web_scan_store.c::web_scan_remove_paths_locked",
    "components/web_server/web_scan_store.c::web_scan_validate_path_locked",
}


def functions(source: str):
    for match in FUNCTION.finditer(source):
        start = match.end() - 1
        depth = 0
        index = start
        while index < len(source):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        name = re.search(r"(\w+)\s*\(", match.group(0)).group(1)
        yield name, source[start:index]


def unguarded_functions() -> set[str]:
    found = set()
    for path in sorted((ROOT / "components").rglob("*.c")):
        relative = path.relative_to(ROOT).as_posix()
        if "/tests/" in relative or "/stubs/" in relative:
            continue
        source = path.read_text(encoding="utf-8", errors="ignore").replace("\r\n", "\n")
        for name, body in functions(source):
            if FS_CALLS.search(body) and "storage_manager_operation_begin" not in body:
                found.add(f"{relative}::{name}")
    return found


def test_the_samples_header_read_of_a_session_summary_holds_the_guard():
    path = ROOT / "components/seismic_recorder/seismic_recorder_json.c"
    body = dict(functions(path.read_text(encoding="utf-8").replace("\r\n", "\n")))["seismic_recorder_json_samples_consistent"]
    begin = body.index("storage_manager_operation_begin")
    assert begin < body.index("fopen(")
    # Every way out after the begin releases it: the failed open and the normal close.
    assert body.count("storage_manager_operation_end()") >= 2
    assert body.index("fclose(") < body.rindex("storage_manager_operation_end()")


def test_no_new_function_touches_the_file_system_without_the_guard():
    unexpected = unguarded_functions() - GUARD_HELD_BY_CALLER
    assert not unexpected, (
        "These functions use the SD card's file system without storage_manager_operation_begin(): "
        + ", ".join(sorted(unexpected))
    )


def test_the_allow_list_has_no_stale_entries():
    stale = GUARD_HELD_BY_CALLER - unguarded_functions()
    assert not stale, "Remove from GUARD_HELD_BY_CALLER (they now take the guard or no longer exist): " + ", ".join(sorted(stale))


def test_the_display_quiesce_never_waits_for_the_lvgl_lock_for_ever():
    """The quiesce runs with the storage operation mutex held. lvgl_port_lock(0) waits for ever, so an LVGL task that waits
    for a task that waits for storage deadlocks them all (board log 2026-11-15 19:28: calculation frozen at 0 %, the
    screen dead). The wait is bounded and storage_manager retries with its mutex released."""
    source = (ROOT / "components/ui/ui_lvgl.c").read_text(encoding="utf-8").replace("\r\n", "\n")
    body = dict(functions(source))["ui_lvgl_quiesce_shared_spi"]
    assert "lvgl_port_lock(0)" not in body
    assert "lvgl_port_lock(UI_LVGL_QUIESCE_LOCK_WAIT_MS)" in body



def test_listing_a_sessions_records_takes_the_guard_for_every_caller():
    """The web page (web_localization_runs_handler), the result screen loader and the 3D preview all list a session's run
    and pick records from their own tasks. The _locked bodies stat/open files, so only the wrappers may call them, and the
    wrappers take the guard (board log 2026-11-15 22:06: unguarded stat from the web handler, spi_hal_setup_trans assert)."""
    path = ROOT / "components/localization_run_store/localization_run_store.c"
    bodies = dict(functions(path.read_text(encoding="utf-8").replace(chr(13) + chr(10), chr(10))))
    for kind in ("list", "list_picks"):
        wrapper = bodies[f"localization_run_store_{kind}_internal"]
        locked = f"localization_run_store_{kind}_locked"
        begin = wrapper.index("storage_manager_operation_begin()")
        call = wrapper.index(locked + "(")
        assert begin < call < wrapper.rindex("storage_manager_operation_end()")
        word = re.compile(r"(?<![A-Za-z0-9_])" + locked + r"\s*[(]")
        callers = [name for name, body in bodies.items() if word.search(body)]
        assert callers == [f"localization_run_store_{kind}_internal"], callers
