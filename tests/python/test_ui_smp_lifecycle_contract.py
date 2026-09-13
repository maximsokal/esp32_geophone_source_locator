"""Static regression contracts for UI/service SMP lifecycle handoff."""

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


def test_ui_runtime_ready_is_atomic_and_published_after_ingress_is_installed():
    internal = read("components/ui/ui_internal.h")
    source = read("components/ui/ui.c")
    init = function_body(source, "esp_err_t ui_init(void)")

    assert "atomic_bool runtime_ready;" in internal
    assert "atomic_init(" in source
    assert "atomic_load_explicit(" in source
    assert "atomic_store_explicit(" in source

    start = init.index("ui_event_dispatcher_start()")
    publish = init.index("&s_context.runtime_ready", start)
    publish_true = init.index("true", publish)
    assert start < publish < publish_true


def test_ui_shutdown_drains_external_ingress_before_runtime_teardown():
    source = read("components/ui/ui.c")
    shutdown = function_body(source, "void ui_shutdown(void)")

    stop_ingress = shutdown.index("ui_event_dispatcher_stop()")
    unregister_services = shutdown.index("ui_unregister_service_callbacks()")
    publish_stopped = shutdown.index("&s_context.runtime_ready", unregister_services)
    publish_false = shutdown.index("false", publish_stopped)
    delete_timer = shutdown.index("ui_delete_refresh_timer()")
    deinit = shutdown.index("ui_lvgl_deinit()")

    assert (
        stop_ingress
        < unregister_services
        < publish_stopped
        < publish_false
        < delete_timer
        < deinit
    )


def test_ui_init_rollback_uses_the_same_callback_barrier_order():
    source = read("components/ui/ui.c")
    rollback = function_body(source, "static void ui_runtime_init_rollback(void)")

    stop_ingress = rollback.index("ui_event_dispatcher_stop()")
    unregister_services = rollback.index("ui_unregister_service_callbacks()")
    publish_stopped = rollback.index("&s_context.runtime_ready", unregister_services)
    publish_false = rollback.index("false", publish_stopped)
    delete_timer = rollback.index("ui_delete_refresh_timer()")

    assert (
        stop_ingress
        < unregister_services
        < publish_stopped
        < publish_false
        < delete_timer
    )
