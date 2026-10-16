from __future__ import annotations

import os
from pathlib import Path
import re


ROOT = Path(os.environ.get("MAX478B_SOURCE_ROOT", Path(__file__).resolve().parents[2]))


def _function_body(relative_path: str, name: str) -> str:
    source = (ROOT / relative_path).read_text(encoding="utf-8")
    signature = re.search(
        rf"(?m)^[ \t]*(?:static\s+)?[^;\n]*\b{re.escape(name)}\s*\(",
        source,
    )
    assert signature is not None, f"missing function {name} in {relative_path}"
    opening = source.find("{", signature.end())
    assert opening >= 0, f"missing body for {name} in {relative_path}"

    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function body: {name}")


def _source_lock_graph() -> set[tuple[str, str]]:
    storage_begin = _function_body(
        "components/storage_manager/storage_manager.c",
        "storage_manager_operation_begin",
    )
    spi_begin = _function_body(
        "components/spi_bus/spi_bus.c",
        "app_spi_bus_sd_operation_begin",
    )
    quiesce = _function_body("components/ui/ui_lvgl.c", "ui_lvgl_quiesce_shared_spi")
    query = _function_body(
        "components/ui/ui_objects_input.c",
        "ui_handle_objects_key_locked",
    )
    storage_input = _function_body(
        "components/ui/ui_storage_input.c",
        "ui_handle_storage_key_locked",
    )
    object_scope = _function_body(
        "components/object_store/object_store_mutations.c",
        "object_store_get_delete_scope",
    )
    ui_init = _function_body("components/ui/ui_lvgl.c", "ui_lvgl_init")

    graph: set[tuple[str, str]] = set()
    storage_take = storage_begin.find("xSemaphoreTakeRecursive")
    storage_spi = storage_begin.find("app_spi_bus_sd_operation_begin")
    if storage_take >= 0 and storage_spi > storage_take:
        graph.add(("storage", "spi"))

    spi_take = spi_begin.find("xSemaphoreTakeRecursive")
    spi_callback = spi_begin.find("s_display_quiesce")
    ui_callback_lock = quiesce.find("lvgl_port_lock")
    callback_registered = "ui_lvgl_quiesce_shared_spi" in ui_init
    if spi_take >= 0 and spi_callback > spi_take and ui_callback_lock >= 0 and callback_registered:
        graph.add(("spi", "lvgl"))

    object_file_operation = _function_body(
        "components/object_store/object_store_state.c",
        "object_store_internal_begin_file_operation",
    )
    object_end_operation = _function_body(
        "components/object_store/object_store_state.c",
        "object_store_internal_end_file_operation",
    )
    object_end_state = _function_body(
        "components/object_store/object_store_state.c",
        "object_store_internal_end_state_operation",
    )
    object_storage = object_file_operation.find("storage_manager_operation_begin")
    query_call = query.find("object_store_get_delete_scope")
    query_enters_file_operation = object_scope.find("object_store_internal_begin_file_operation")
    object_lock = object_file_operation.find("xSemaphoreTake")
    query_unlock = query.rfind("lvgl_port_unlock", 0, query_call)
    query_relock = query.find("lvgl_port_lock", query_call)
    if (
        object_storage >= 0
        and query_enters_file_operation >= 0
        and query_call >= 0
        and not (
            query_unlock >= 0 and query_relock > query_call
        )
    ):
        graph.add(("lvgl", "storage"))

    ready_query = storage_input.find("storage_manager_is_ready")
    ready_unlock = storage_input.rfind("lvgl_port_unlock", 0, ready_query)
    ready_relock = storage_input.find("lvgl_port_lock", ready_query)
    if ready_query >= 0 and not (ready_unlock >= 0 and ready_relock > ready_query):
        graph.add(("lvgl", "storage"))
    if (
        object_storage >= 0
        and object_lock > object_storage
        and object_end_operation.find("object_store_internal_end_state_operation") >= 0
        and object_end_state.find("xSemaphoreGive") >= 0
    ):
        graph.add(("storage", "object_store"))

    web_refresh = _function_body(
        "components/web_server/web_status_json_objects.c",
        "web_status_json_objects_refresh",
    )
    refresh_take = web_refresh.find("xSemaphoreTake")
    reserve_refresh = web_refresh.find("web_status_cache_arbitration_try_reserve_refresh")
    object_snapshot = web_refresh.find("object_store_list_profiles_responsive")
    if refresh_take >= 0 and reserve_refresh > refresh_take:
        graph.add(("web_refresh", "web_status_arbitration"))
    if refresh_take >= 0 and object_snapshot > refresh_take:
        graph.add(("web_refresh", "storage"))

    return graph


def _has_cycle(graph: set[tuple[str, str]]) -> bool:
    pending = {node for edge in graph for node in edge}
    for start in pending:
        stack = [(start, {start})]
        while stack:
            node, visited = stack.pop()
            for left, right in graph:
                if left != node:
                    continue
                if right in visited:
                    return True
                stack.append((right, visited | {right}))
    return False


def test_storage_spi_lvgl_graph_is_acyclic() -> None:
    graph = _source_lock_graph()
    assert graph == {
        ("storage", "spi"),
        ("spi", "lvgl"),
        ("storage", "object_store"),
        ("web_refresh", "web_status_arbitration"),
        ("web_refresh", "storage"),
    }
    assert not _has_cycle(graph)


def test_ui_releases_lvgl_around_delete_scope_storage_query() -> None:
    body = _function_body(
        "components/ui/ui_objects_input.c",
        "ui_handle_objects_key_locked",
    )
    query = body.find("object_store_get_delete_scope")
    begin = body.find("ui_storage_query_begin")
    unlock = body.rfind("lvgl_port_unlock", 0, query)
    relock = body.find("lvgl_port_lock", query)
    end = body.find("ui_storage_query_end", relock)
    assert 0 <= begin < unlock < query < relock < end


def test_ui_releases_lvgl_around_storage_readiness_query() -> None:
    body = _function_body(
        "components/ui/ui_storage_input.c",
        "ui_handle_storage_key_locked",
    )
    query = body.find("storage_manager_is_ready")
    begin = body.find("ui_storage_query_begin")
    unlock = body.rfind("lvgl_port_unlock", 0, query)
    relock = body.find("lvgl_port_lock", query)
    end = body.find("ui_storage_query_end", relock)
    assert 0 <= begin < unlock < query < relock < end

    renderer = _function_body("components/ui/ui_storage.c", "ui_render_storage_locked")
    assert "storage_manager_is_ready" not in renderer


def test_preview_finishes_storage_reads_before_taking_lvgl() -> None:
    worker = _function_body("components/ui/ui_scan_preview.c", "preview_loader_task")
    storage_load = worker.find("load_session(&request, &result)")
    lvgl_lock = worker.find("lvgl_port_lock", storage_load)
    lvgl_unlock = worker.find("lvgl_port_unlock", lvgl_lock)
    assert 0 <= storage_load < lvgl_lock < lvgl_unlock

    for callback in ("file_size_callback", "file_chunk_callback"):
        body = _function_body("components/ui/ui_scan_preview.c", callback)
        begin = body.find("storage_manager_operation_begin")
        end = body.find("storage_manager_operation_end")
        assert 0 <= begin < end


def test_synchronous_ui_storage_call_sites_match_the_checked_graph() -> None:
    expected = {
        "components/ui/ui_objects_input.c": {
            "object_store_get_delete_scope": 1,
        },
        "components/ui/ui_storage_input.c": {
            "storage_manager_is_ready": 1,
        },
        "components/ui/ui_scan_preview.c": {
            "storage_manager_operation_begin": 2,
            "storage_manager_operation_end": 2,
        },
    }
    actual: dict[str, dict[str, int]] = {}
    call_pattern = re.compile(r"\b((?:object_store|storage_manager)_[A-Za-z0-9_]+)\s*\(")
    for source in (ROOT / "components/ui").glob("*.c"):
        matches = call_pattern.findall(source.read_text(encoding="utf-8"))
        if matches:
            counts = {name: matches.count(name) for name in set(matches)}
            actual[source.relative_to(ROOT).as_posix()] = counts
    assert actual == expected


def test_shutdown_closes_and_drains_the_unlocked_query_gate() -> None:
    init = _function_body("components/ui/ui.c", "ui_init")
    shutdown = _function_body("components/ui/ui.c", "ui_shutdown")
    gate_open = init.find("ui_storage_query_gate_open")
    runtime_publish = init.find("memory_order_release")
    gate_close = shutdown.find("ui_storage_query_gate_close_and_wait")
    lvgl_deinit = shutdown.find("ui_lvgl_deinit")
    assert 0 <= gate_open < runtime_publish
    assert 0 <= gate_close < lvgl_deinit


def test_storage_service_command_gate_is_nonblocking() -> None:
    body = _function_body(
        "components/storage_service/storage_service.c",
        "storage_service_submit_active",
    )
    assert re.search(r"xSemaphoreTake\s*\(\s*s_command_gate\s*,\s*0\s*\)", body)
