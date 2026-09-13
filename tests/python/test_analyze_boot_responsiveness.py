from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = REPO_ROOT / "tools" / "analyze_boot_responsiveness.py"

_spec = importlib.util.spec_from_file_location("analyze_boot_responsiveness", ANALYZER_PATH)
assert _spec is not None and _spec.loader is not None
analyzer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = analyzer
_spec.loader.exec_module(analyzer)


def healthy_log() -> str:
    lines = [
        "I APP: BOOT_TIMING: startup_total=3200 ms",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=START",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_PROGRESS object=OBJ_0001 index=1 total=2 removed=0 elapsed_ms=120",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_PROGRESS object=OBJ_0002 index=2 total=2 removed=0 elapsed_ms=135",
    ]

    names = {1: "LEFT", 2: "UP", 3: "DOWN", 4: "RIGHT", 5: "OK"}
    for key in range(1, 6):
        name = names[key]
        lines.extend(
            [
                f"I KEYPAD: KEYPAD_RAW_TRANSITION raw={100 * key} from=NONE to={name}",
                f"I KEYPAD: KEYPAD_BUTTON_PRESS key={name} value={key}",
                f"I APP_EVENTS: KEYPAD_EVENT_POST key={key}",
                f"I UI_EVENT: KEYPAD_EVENT_DISPATCH key={key} age_ms={10 + key}",
                f"I UI_EVENT: UI_KEY_RESULT key={key} screen_before=0 screen_after={1 if key == 4 else 0} selected_before=0 selected_after={key % 5}",
                f"I KEYPAD: KEYPAD_RAW_TRANSITION raw=4095 from={name} to=NONE",
            ]
        )

    lines.extend(
        [
            "I RUNTIME: RUNTIME_TASK rank=1 name=web_status_cache core=0 priority_current=1 priority_base=1 inherited=0",
            "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=STOP",
            "I WEB: WEB_STATUS_CACHE refresh=done",
        ]
    )
    return "\n".join(lines) + "\n"


def test_healthy_log_passes_full_key_chain():
    result = analyzer.analyze(healthy_log())

    assert result.status == "PASS"
    assert result.missing == ()
    assert result.violations == ()
    assert result.startup_ms == 3200
    assert result.max_recovery_window_ms == 135
    assert result.raw_keys == (1, 2, 3, 4, 5)
    assert result.button_keys == (1, 2, 3, 4, 5)
    assert result.posted_keys == (1, 2, 3, 4, 5)
    assert result.dispatched_keys == (1, 2, 3, 4, 5)
    assert result.ui_result_keys == (1, 2, 3, 4, 5)


def test_dispatch_without_ui_result_is_blocked_not_pass():
    content = healthy_log().replace(
        "I UI_EVENT: UI_KEY_RESULT key=5 screen_before=0 screen_after=0 selected_before=0 selected_after=0\n",
        "",
    )

    result = analyzer.analyze(content)

    assert result.status == "BLOCKED"
    assert any(item == "UI_KEY_RESULT missing=5" for item in result.missing)
    assert result.violations == ()


def test_slow_startup_and_recovery_are_failures():
    content = healthy_log().replace("startup_total=3200", "startup_total=6001")
    content = content.replace("elapsed_ms=135", "elapsed_ms=251")

    result = analyzer.analyze(content)

    assert result.status == "FAIL"
    assert any("startup_total=6001ms" in item for item in result.violations)
    assert any("recovery_window=251ms" in item for item in result.violations)


def test_stale_shutdown_and_panic_markers_fail():
    content = healthy_log() + "\n".join(
        [
            "W UI_EVENT: Dropped stale keypad navigation age_ms=300 total=1",
            "W APP: Safe shutdown started",
            "E cpu: Guru Meditation Error: Core 0 panic'ed",
        ]
    )

    result = analyzer.analyze(content)

    assert result.status == "FAIL"
    assert result.stale_drop_count == 1
    assert result.safe_shutdown_count == 1
    assert result.failure_marker_count == 1


def test_button_name_value_mismatch_fails():
    content = healthy_log().replace(
        "KEYPAD_BUTTON_PRESS key=LEFT value=1",
        "KEYPAD_BUTTON_PRESS key=LEFT value=2",
    )

    result = analyzer.analyze(content)

    assert result.status == "FAIL"
    assert any("inconsistent name=LEFT value=2" in item for item in result.violations)


def test_priority_inheritance_consistency_is_checked():
    inherited = healthy_log().replace(
        "priority_current=1 priority_base=1 inherited=0",
        "priority_current=23 priority_base=1 inherited=1",
    )
    result = analyzer.analyze(inherited)
    assert result.status == "PASS"
    assert result.inherited_priority_observed is True

    inconsistent = inherited.replace("inherited=1", "inherited=0")
    bad = analyzer.analyze(inconsistent)
    assert bad.status == "FAIL"
    assert any("inconsistent with current=23 base=1" in item for item in bad.violations)


def test_priority_base_regression_fails():
    content = healthy_log().replace(
        "priority_current=1 priority_base=1 inherited=0",
        "priority_current=2 priority_base=2 inherited=0",
    )

    result = analyzer.analyze(content)

    assert result.status == "FAIL"
    assert any("priority_base=2 expected=1" in item for item in result.violations)


def test_archive_ordering_is_checked():
    content = healthy_log()
    content = content.replace(
        "I APP: BOOT_TIMING: startup_total=3200 ms\nI OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=START",
        "I OBJECT_RECOVERY: OBJECT_RECOVERY_BACKGROUND state=START\nI APP: BOOT_TIMING: startup_total=3200 ms",
    )

    result = analyzer.analyze(content)

    assert result.status == "FAIL"
    assert "background recovery started before startup_total" in result.violations


def test_missing_recovery_or_cache_evidence_is_blocked():
    content = healthy_log()
    content = "\n".join(
        line
        for line in content.splitlines()
        if "OBJECT_RECOVERY_PROGRESS" not in line
        and "OBJECT_RECOVERY_BACKGROUND state=STOP" not in line
        and "WEB_STATUS_CACHE refresh=done" not in line
    )

    result = analyzer.analyze(content)

    assert result.status == "BLOCKED"
    assert "OBJECT_RECOVERY_PROGRESS" in result.missing
    assert "OBJECT_RECOVERY_BACKGROUND state=STOP" in result.missing
    assert "WEB_STATUS_CACHE refresh=done" in result.missing
