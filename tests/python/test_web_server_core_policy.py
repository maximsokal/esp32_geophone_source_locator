"""Web server task placement policy: Core 1 below both acquisition priorities.

Configuration contract (not a behavioural oracle): the HTTP and HTTPS server
configs must take the core and priority from app_task_config.h, and the
priority must stay below the geophone ADC and AD7606 acquisition tasks.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = (ROOT / "components/app_config/include/app_task_config.h").read_text(encoding="utf-8")
SERVER = (ROOT / "components/web_server/web_server.c").read_text(encoding="utf-8")


def _define(name: str) -> str:
    match = re.search(rf"#define\s+{name}\s+(\S+)", CONFIG)
    assert match, name
    return match.group(1)


def test_web_server_runs_on_realtime_core_below_acquisition_priorities():
    assert _define("APP_TASK_CORE_WEB_SERVER") == "APP_TASK_CORE_REALTIME"
    priority = int(_define("APP_TASK_PRIORITY_WEB_SERVER"))
    assert priority < int(_define("APP_TASK_PRIORITY_GEOPHONE_ADC"))
    assert priority < int(_define("APP_TASK_PRIORITY_AD7606"))


def test_http_and_https_servers_use_the_central_placement_constants():
    assert "config.core_id = APP_TASK_CORE_WEB_SERVER;" in SERVER
    assert "config.task_priority = APP_TASK_PRIORITY_WEB_SERVER;" in SERVER
    assert "config.httpd.core_id = APP_TASK_CORE_WEB_SERVER;" in SERVER
    assert "config.httpd.task_priority = APP_TASK_PRIORITY_WEB_SERVER;" in SERVER
