"""Source contracts for MAX-11 task-priority diagnostics.

The old board log showed web_status_cache at effective priority 23 even though
its configured priority is 1. These contracts keep the configured creation
priority unchanged and require runtime logs to expose current vs base priority
so live HIL can distinguish mutex inheritance from a configuration bug.
"""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
TASK_CONFIG = (
    REPO_ROOT / "components" / "app_config" / "include" / "app_task_config.h"
).read_text(encoding="utf-8")
WEB_STATUS = (
    REPO_ROOT / "components" / "web_server" / "web_status_json_objects.c"
).read_text(encoding="utf-8")
RUNTIME = (
    REPO_ROOT / "components" / "runtime_observability" / "runtime_observability.c"
).read_text(encoding="utf-8")


def test_web_status_cache_remains_low_priority_by_configuration():
    assert "#define APP_TASK_PRIORITY_WEB_STATUS_CACHE 1" in TASK_CONFIG


def test_web_status_cache_creation_uses_configured_priority_symbol():
    start = WEB_STATUS.index("esp_err_t web_status_json_objects_init(void)")
    end = WEB_STATUS.index("void web_status_json_objects_shutdown(void)")
    init_body = WEB_STATUS[start:end]

    assert '"web_status_cache"' in init_body
    assert "xTaskCreatePinnedToCoreWithCaps(" in init_body
    assert "APP_TASK_PRIORITY_WEB_STATUS_CACHE" in init_body
    assert "APP_TASK_CORE_SERVICES" in init_body


def test_runtime_snapshot_exposes_priority_inheritance_evidence():
    assert "priority_current=%u priority_base=%u inherited=%u" in RUNTIME
    assert "status->uxCurrentPriority" in RUNTIME
    assert "status->uxBasePriority" in RUNTIME
    assert "status->uxCurrentPriority > status->uxBasePriority ? 1U : 0U" in RUNTIME


def test_no_hardcoded_priority_23_workaround_is_present():
    assert "APP_TASK_PRIORITY_WEB_STATUS_CACHE 23" not in TASK_CONFIG
