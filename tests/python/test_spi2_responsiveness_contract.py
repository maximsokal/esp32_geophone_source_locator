"""Architecture regressions for the shared TFT/microSD SPI2 bus.

These checks are intentionally structural. The physical HIL test remains the
source of latency numbers, but a future refactor must not accidentally put a
socket wait or an arbitrarily long waveform reduction back inside a storage/TFT
quiesce window, starve the recorder, or steal esp_lvgl_port's LCD completion
callback again.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def _function_body(source: str, name: str) -> str:
    marker = f"{name}("
    start = source.find(marker)
    assert start >= 0, f"missing function {name}"
    brace = source.find("{", start)
    assert brace >= 0, f"missing function body for {name}"

    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function {name}")


def _priority(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", source, re.MULTILINE)
    assert match is not None, f"missing numeric priority {name}"
    return int(match.group(1))


def test_scan_socket_waits_are_outside_storage_windows() -> None:
    source = _read("components/web_server/web_scan_store.c")

    save = _function_body(source, "web_scan_save")
    assert "httpd_req_recv" in save
    assert "web_scan_write_chunk" in save
    assert "storage_manager_operation_begin" not in save
    assert "web_scan_priority_enter" in save
    assert "web_scan_priority_leave" in save

    write_chunk = _function_body(source, "web_scan_write_chunk")
    assert write_chunk.index("storage_manager_operation_begin") < write_chunk.index("fwrite")
    assert write_chunk.index("fwrite") < write_chunk.index("storage_manager_operation_end")

    load = _function_body(source, "web_scan_load")
    assert "web_scan_read_chunk" in load
    assert "httpd_resp_send_chunk" in load
    assert "storage_manager_operation_begin" not in load
    assert load.index("web_scan_read_chunk") < load.index("httpd_resp_send_chunk")
    assert "web_scan_priority_enter" in load
    assert "web_scan_priority_leave" in load

    read_chunk = _function_body(source, "web_scan_read_chunk")
    assert read_chunk.index("storage_manager_operation_begin") < read_chunk.index("fread")
    assert read_chunk.index("fread") < read_chunk.index("storage_manager_operation_end")

    priority = _function_body(source, "web_scan_priority_enter")
    assert "APP_TASK_PRIORITY_STORAGE_SERVICE" in priority
    assert "vTaskPrioritySet" in priority


def test_bulk_scan_never_outranks_recorder_and_lvgl_yields_to_both() -> None:
    task_config = _read("components/app_config/include/app_task_config.h")

    scan_priority = _priority(task_config, "APP_TASK_PRIORITY_STORAGE_SERVICE")
    recorder_priority = _priority(task_config, "APP_TASK_PRIORITY_SEISMIC_RECORDER")
    lvgl_priority = _priority(task_config, "APP_TASK_PRIORITY_LVGL")

    # The scan handler lowers itself to STORAGE_SERVICE while moving the large
    # payload. Recorder must be able to time-slice at that level. The screen is
    # the least important consumer: it runs below both, and redraws at 4 FPS at
    # most (ui_perf_policy.h); its recursive display lock lends priority to a
    # storage task that waits for it.
    assert recorder_priority >= scan_priority
    assert lvgl_priority < scan_priority
    assert lvgl_priority < recorder_priority


def test_web_status_uses_cooperative_storage_snapshots() -> None:
    source = _read("components/web_server/web_status_json_objects.c")
    refresh = _function_body(source, "web_status_json_objects_refresh")

    assert "object_store_list_profiles_responsive" in refresh
    assert "seismic_recorder_list_sessions_responsive" in refresh
    assert "object_store_list_profiles_with_defaults" not in refresh
    assert "seismic_recorder_list_sessions(" not in refresh

    object_cmake = _read("components/object_store/CMakeLists.txt")
    recorder_cmake = _read("components/seismic_recorder/CMakeLists.txt")
    assert '"object_store_responsive.c"' in object_cmake
    assert '"seismic_recorder_list_responsive.c"' in recorder_cmake


def test_saved_waveform_reduction_never_holds_spi2_for_the_whole_range() -> None:
    reader_source = _read("components/seismic_recorder/seismic_recorder_reader.c")
    internal = _read("components/seismic_recorder/seismic_recorder_internal.h")
    waveform = _read("components/seismic_recorder/seismic_recorder_waveform.c")
    task_config = _read("components/app_config/include/app_task_config.h")

    public_read = _function_body(reader_source, "seismic_recorder_read_session")
    assert "seismic_recorder_waveform_read_source" in public_read
    assert "heap_caps_malloc" in public_read
    assert "SEISMIC_RECORDER_READ_STAGING_FRAMES" in public_read
    assert "storage_manager_operation_begin" not in public_read

    bounded_read = _function_body(reader_source, "seismic_recorder_reader_read_at")
    lock = bounded_read.index("storage_manager_operation_begin")
    open_file = bounded_read.index("fopen")
    seek = bounded_read.index("fseek")
    read = bounded_read.index("fread")
    close = bounded_read.index("fclose")
    unlock = bounded_read.index("storage_manager_operation_end")
    assert lock < open_file < seek < read < close < unlock

    # No FILE handle is retained between windows while FATFS_FS_LOCK=0. The
    # generic reducer receives one PSRAM chunk at a time after read_at returns.
    assert "FILE *file" not in public_read
    assert "SEISMIC_RECORDER_READ_STAGING_FRAMES  1024U" in internal

    reducer = _function_body(waveform, "seismic_recorder_waveform_read_source")
    assert "while (processed < frame_count)" in reducer
    assert "read_at(" in reducer
    assert "staging[chunk_index]" in reducer
    assert "storage_manager_operation_begin" not in reducer

    # Every potentially many-window reduction path (retained cache, LOD hybrid,
    # and raw fallback) lowers HTTPD to storage-service priority while reducing.
    priority_enter = _function_body(reader_source, "seismic_recorder_reader_priority_enter")
    priority_leave = _function_body(reader_source, "seismic_recorder_reader_priority_leave")
    assert "APP_TASK_PRIORITY_STORAGE_SERVICE" in priority_enter
    assert "uxTaskPriorityGet(NULL)" in priority_enter
    assert "vTaskPrioritySet(NULL, APP_TASK_PRIORITY_STORAGE_SERVICE)" in priority_enter
    assert "vTaskPrioritySet(NULL, guard.original_priority)" in priority_leave
    assert public_read.count("seismic_recorder_reader_priority_enter()") == 3
    assert public_read.count("seismic_recorder_reader_priority_leave(priority_guard)") == 3

    storage_priority = _priority(task_config, "APP_TASK_PRIORITY_STORAGE_SERVICE")
    recorder_priority = _priority(task_config, "APP_TASK_PRIORITY_SEISMIC_RECORDER")
    lvgl_priority = _priority(task_config, "APP_TASK_PRIORITY_LVGL")
    assert recorder_priority >= storage_priority
    assert lvgl_priority < storage_priority


def test_lvgl_port_keeps_lcd_completion_callback_ownership() -> None:
    source = _read("components/ui/ui_lvgl.c")

    assert "esp_lcd_panel_io_register_event_callbacks" not in source
    assert "lv_display_set_flush_wait_cb" not in source
    assert "esp_lcd_panel_io_tx_param" in source
    assert "ui_lvgl_drain_panel_io_locked" in source
