#!/usr/bin/env python3
"""Run short ESP-EMU smoke scenarios against the emulator-profile firmware."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path, PureWindowsPath
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EMU = "/opt/esp-emu-dl/esp-emu-0.49.0-x86_64-unknown-linux-gnu/esp-emu"
DEFAULT_IMAGE = ".test-build/emu/merged_emu.bin"
DEFAULT_ELF = ".test-build/idf-emulator/esp32_geophone_source_locator.elf"
DEFAULT_WDT_IMAGE = ".test-build/emu/merged_emu_wdt.bin"
DEFAULT_WDT_ELF = ".test-build/idf-emulator-wdt/esp32_geophone_source_locator.elf"
DEFAULT_WDT_BUILD_DIR = ".test-build/idf-emulator-wdt"
DEFAULT_REC_IMAGE = ".test-build/emu/merged_emu_rec.bin"
DEFAULT_REC_ELF = ".test-build/idf-emulator-rec/esp32_geophone_source_locator.elf"
DEFAULT_REC_BUILD_DIR = ".test-build/idf-emulator-rec"
DEFAULT_OUT_DIR = ".test-build/emu-smoke"
FLASH_SIZE = 16 * 1024 * 1024
FLASH_OFFSETS = {"bootloader": 0x0, "partition_table": 0x8000, "app": 0x10000}
SCENARIOS = ("S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8")
ONB_SCENARIOS = frozenset(("S5", "S6", "S7", "S8"))
ONB_TIMEOUT_SECONDS = 240
ONB_OUTER_TIMEOUT_SECONDS = 300
LIMITATIONS = (
    "ESP-EMU cannot prove SD, AD7606 BUSY, TCA9548A, INA3221, or hardware timing. "
    "TFT/LVGL and ADC are not modelled and remain unverified. "
    "S8 checks watchdog stability only. S9 proves the firmware's recovery path from an interrupt-WDT panic under emulation; "
    "it does not prove silicon watchdog timing, and production task-WDT behavior is not covered because "
    "CONFIG_ESP_TASK_WDT_PANIC is unset."
)

_FATAL_LINE = re.compile(
    r"Guru Meditation|Backtrace\b|Firmware abort|\babort(?:ed|ing)?\b",
    re.IGNORECASE,
)
_RESET_BOOT_ID = re.compile(
    r"\bRESET_REASON\b[^\r\n]*\bboot_id=([0-9A-Fa-fx]+)", re.IGNORECASE
)
_RESET_REASON_LINE = re.compile(r"\bRESET_REASON\b", re.IGNORECASE)
_LOG_TIMESTAMP = re.compile(r"\(\s*(\d+)\s*\)")
_CORE_DEFINE = re.compile(
    r"^\s*#\s*define\s+(APP_TASK_CORE_[A-Z0-9_]+)\s+([^/\s]+)",
    re.MULTILINE,
)
_TRACE_CORE_MACROS = {
    "seismic_test": "APP_TASK_CORE_REALTIME",
    "httpd": "APP_TASK_CORE_WEB_SERVER",
    # Affinities come from app_task_config.h; Wi-Fi follows README.md:156-177's
    # Core 0 service/network policy.
    "wifi": "APP_TASK_CORE_SERVICES",
    "workflow_poll": "APP_TASK_CORE_SERVICES",
    "nrf_topology": "APP_TASK_CORE_SERVICES",
    "runtime_observability": "APP_TASK_CORE_SERVICES",
    "ina3221": "APP_TASK_CORE_SERVICES",
    "localization_job": "APP_TASK_CORE_REALTIME",
}
_REQUIRED_TRACE_TASKS = frozenset(
    ("seismic_test", "httpd", "wifi", "workflow_poll", "nrf_topology", "runtime_observa", "ina3221")
)
_WATCHDOG_MARKERS = (
    "Interrupt wdt timeout",
    "Task watchdog got triggered",
    "task_wdt:",
    "Guru Meditation",
    "Backtrace:",
    "abort()",
)


def _panic_lines(log_text: str) -> list[str]:
    return [
        line
        for line in log_text.splitlines()
        if "panic intercept" not in line.lower() and _FATAL_LINE.search(line)
    ]


def _common_reasons(log_text: str, extra: dict[str, Any] | None) -> list[str]:
    reasons = []
    panic_lines = _panic_lines(log_text)
    if panic_lines:
        reasons.append(f"panic/fatal marker: {panic_lines[0].strip()}")
    for line in log_text.splitlines():
        for marker in ("nRF topology service unavailable:",
                       "nRF emergency-stop monitor unavailable:",
                       "Pod status bridge unavailable:"):
            if marker in line:
                reasons.append(f"service startup failure: {line[line.index(marker):].strip()}")
    if extra:
        if extra.get("run_error"):
            reasons.append(f"runner error: {extra['run_error']}")
        return_code = extra.get("wsl_returncode")
        if return_code is not None and return_code != 0:
            reasons.append(f"WSL scenario command exited with {return_code}")
    return reasons


def evaluate_s1(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate boot and 60-second stability markers."""
    reasons = _common_reasons(log_text, extra)
    for marker in (
        "Hardware bring-up completed",
        "Display: SKIPPED",
        "Keypad: SKIPPED",
        "BOOT_NO_STORAGE",
    ):
        if marker not in log_text:
            reasons.append(f"missing required log marker: {marker}")
    stages = (
        "TEST_STARTUP_HEAP: pressure_ready",
        "TEST_STARTUP_HEAP: control_rejected",
        "TEST_STARTUP_HEAP service=topology result=ESP_OK",
        "TEST_STARTUP_HEAP service=estop result=ESP_OK",
        "TEST_STARTUP_HEAP service=pod_bridge result=ESP_OK",
        "TEST_STARTUP_HEAP: release",
    )
    positions = []
    for stage in stages:
        matches = list(re.finditer(re.escape(stage) + r"(?=\s|$)", log_text))
        if len(matches) != 1:
            reasons.append(f"expected exactly one marker: {stage}")
        positions.append(matches[0].start() if matches else -1)
    if all(position >= 0 for position in positions) and positions != sorted(positions):
        reasons.append("startup heap markers are out of order")
    for stage in stages[:2]:
        for line in log_text.splitlines():
            if stage not in line:
                continue
            free = re.search(r"\bfree_internal=(\d+)(?=\s|$)", line)
            largest = re.search(r"\blargest_internal=(\d+)(?=\s|$)", line)
            if not free or not largest or not 4096 <= int(free.group(1)) <= 6144 or int(largest.group(1)) > 3584:
                reasons.append(f"invalid startup heap metrics: {line.strip()}")
    if "TEST_STARTUP_HEAP: FAIL" in log_text:
        reasons.append("startup heap pressure hook reported FAIL")
    if extra is not None and "duration_seconds" in extra:
        try:
            duration = float(extra["duration_seconds"])
        except (TypeError, ValueError):
            reasons.append("measured run duration is invalid")
        else:
            if duration < 60:
                reasons.append(f"run lasted {duration:.1f}s; at least 60s is required")
    return not reasons, reasons


def evaluate_s2(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate reset-channel acknowledgement and evidence of two boots."""
    reasons = _common_reasons(log_text, extra)
    if log_text.count("Hardware bring-up completed") < 2:
        reasons.append("expected two Hardware bring-up completed markers")
    boot_ids = _RESET_BOOT_ID.findall(log_text)
    if len(boot_ids) < 2:
        reasons.append("expected two RESET_REASON lines containing boot_id")
    elif len(set(boot_ids)) < 2:
        reasons.append("RESET_REASON boot_id values did not change across reset")
    control = (extra or {}).get("control", {})
    reset_reply = (control.get("reset") if isinstance(control, dict) else None) or (
        (extra or {}).get("control_reply")
    )
    if str(reset_reply).strip().lower() != "ok":
        reasons.append("control channel did not acknowledge reset with 'ok'")
    return not reasons, reasons


def evaluate_s3(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate the 4 MB PSRAM startup case."""
    reasons = _common_reasons(log_text, extra)
    if "PSRAM configured as 4096 KB" not in log_text:
        reasons.append("missing PSRAM configured as 4096 KB marker")
    if "Hardware bring-up completed" not in log_text:
        reasons.append("missing Hardware bring-up completed marker")
    return not reasons, reasons


def evaluate_s4(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate HTTP bootstrap redirects and the intentionally absent HTTP status API."""
    reasons = _common_reasons(log_text, extra)
    if "HTTP bootstrap server started" not in log_text:
        reasons.append("missing HTTP bootstrap server started marker")
    responses = (extra or {}).get("http")
    if not isinstance(responses, dict):
        reasons.append("HTTP probe results are missing")
        return not reasons, reasons

    for path in ("/setup", "/"):
        response = responses.get(path)
        status = response.get("status") if isinstance(response, dict) else None
        if status != 302:
            reasons.append(f"GET {path} returned {status!r}; expected HTTP 302")
        body = response.get("body", "") if isinstance(response, dict) else ""
        if "Install and confirm the GeoLocator Root CA" not in str(body):
            reasons.append(f"GET {path} body is missing the root CA confirmation text")

    status_response = responses.get("/api/status")
    status_code = status_response.get("status") if isinstance(status_response, dict) else None
    if status_code != 404:
        reasons.append(f"GET /api/status returned {status_code!r}; expected HTTP 404")
    return not reasons, reasons


def _onb_results(extra: dict[str, Any] | None, reasons: list[str]) -> dict[str, Any]:
    result = (extra or {}).get("onb")
    if not isinstance(result, dict):
        reasons.append("ONB client results are missing")
        return {}
    if result.get("client_error"):
        reasons.append(f"ONB client error: {result['client_error']}")
    return result


def _http_status(result: Any) -> Any:
    return result.get("status") if isinstance(result, dict) else None


def evaluate_s5(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate setup onboarding, HTTPS status, and acquisition start."""
    reasons = _common_reasons(log_text, extra)
    onb = _onb_results(extra, reasons)
    http = onb.get("http")
    if not isinstance(http, dict):
        reasons.append("ONB HTTP onboarding results are missing")
        http = {}

    for path in ("/setup/ca-guide", "/setup/ca-download", "/setup/ca?installed=1", "/setup"):
        response = http.get(path)
        if _http_status(response) != "200":
            reasons.append(f"GET {path} returned {_http_status(response)!r}; expected HTTP 200")
    ca_download = http.get("/setup/ca-download")
    if isinstance(ca_download, dict):
        if ca_download.get("pem") is not True:
            reasons.append("GET /setup/ca-download did not save a PEM certificate")
        if ca_download.get("bytes") != 1948:
            reasons.append(
                f"GET /setup/ca-download saved {ca_download.get('bytes')!r} bytes; expected 1948"
            )

    wifi_setup = http.get("POST /api/wifi/setup")
    wifi_status = _http_status(wifi_setup)
    if wifi_status != "200":
        detail = " (CA acknowledgement or setup mode was not accepted)" if wifi_status == "403" else ""
        reasons.append(
            f"POST /api/wifi/setup returned {wifi_status!r}; expected HTTP 200{detail}"
        )

    status = onb.get("status_before_reset")
    if _http_status(status) != "200":
        reasons.append(
            f"HTTPS /api/status returned {_http_status(status)!r} after Wi-Fi setup; expected HTTP 200 within 60s"
        )
    elif not isinstance(status, dict) or not isinstance(status.get("elapsed_seconds"), (int, float)) or status["elapsed_seconds"] > 60:
        reasons.append("HTTPS /api/status did not return HTTP 200 within 60 seconds of the Wi-Fi setup POST")
    else:
        payload = status.get("json")
        if not isinstance(payload, dict):
            reasons.append("HTTPS /api/status did not return a JSON object")
        else:
            measurement = payload.get("measurement")
            if not isinstance(measurement, dict) or measurement.get("state") != "LIVE":
                reasons.append("HTTPS /api/status measurement.state was not LIVE")
            system = payload.get("system")
            if not isinstance(system, dict) or not isinstance(system.get("uptime_ms"), int):
                reasons.append("HTTPS /api/status is missing system.uptime_ms")
            runtime = payload.get("runtime")
            heap = runtime.get("heap") if isinstance(runtime, dict) else None
            if not isinstance(heap, dict) or not isinstance(heap.get("free_internal"), int):
                reasons.append("HTTPS /api/status is missing runtime.heap.free_internal")
            tasks = runtime.get("tasks") if isinstance(runtime, dict) else None
            acquisition = tasks.get("acquisition") if isinstance(tasks, dict) else None
            if not isinstance(acquisition, dict) or not isinstance(acquisition.get("observed"), bool):
                reasons.append("HTTPS /api/status is missing runtime.tasks.acquisition.observed")

    control = onb.get("acquisition_start")
    if _http_status(control) != "200":
        reasons.append(
            f"POST /api/control acquisition_start returned {_http_status(control)!r}; expected HTTP 200"
        )
    elif not isinstance(control, dict) or not isinstance(control.get("json"), dict) or control["json"].get("ok") is not True or control["json"].get("action") != "acquisition_start":
        reasons.append("POST /api/control acquisition_start did not return {ok:true, action:acquisition_start}")

    if "Starting HTTPS server on port 443" not in log_text:
        reasons.append("missing HTTPS-server-start log line")
    return not reasons, reasons


def _reset_log_parts(
    log_text: str, reset_log_line: int | None = None
) -> tuple[list[str], list[str], list[str]]:
    lines = log_text.splitlines()
    if isinstance(reset_log_line, int) and 0 <= reset_log_line <= len(lines):
        reset_position = reset_log_line
    else:
        reset_positions = [
            index for index, line in enumerate(lines) if _RESET_REASON_LINE.search(line)
        ]
        reset_position = reset_positions[1] if len(reset_positions) >= 2 else len(lines)
    before_reset = lines[:reset_position]
    after_reset = lines[reset_position:]
    boot_ids_before = _RESET_BOOT_ID.findall("\n".join(before_reset))
    boot_ids_after = _RESET_BOOT_ID.findall("\n".join(after_reset))
    return before_reset, after_reset, boot_ids_before + boot_ids_after


def _port80_is_not_setup(result: Any) -> bool:
    if not isinstance(result, dict):
        return False
    status = result.get("status")
    error = str(result.get("error", "")).lower()
    if status is None:
        no_listener_errors = (
            "connection refused",
            "connection reset",
            "remote end closed connection without response",
            "remotedisconnected",
            "empty reply",
        )
        return any(marker in error for marker in no_listener_errors)
    body = str(result.get("body", "")).lower()
    location = str(result.get("location", "")).lower()
    if "/setup" in location:
        return False
    if "install and confirm the geolocator root ca" in body:
        return False
    if re.search(r"\bname\s*=\s*[\"']?(?:ssid|password)\b", body, re.IGNORECASE):
        return False
    if status == 200 and not body.strip():
        return False
    return True


def evaluate_s7(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate saved-credential STA boot and port-80 shutdown after reset."""
    reasons = _common_reasons(log_text, extra)
    onb = _onb_results(extra, reasons)
    if onb.get("reset_sent") is not True:
        reasons.append("reset was not sent over the control channel")

    reset_line = onb.get("reset_log_line")
    _, after_reset, _ = _reset_log_parts(
        log_text, reset_line if isinstance(reset_line, int) else None
    )
    after_text = "\n".join(after_reset)
    boot_ids_after = _RESET_BOOT_ID.findall(after_text)
    boot_ids_all = _RESET_BOOT_ID.findall(log_text)
    if len(boot_ids_all) < 2:
        reasons.append("expected a second RESET_REASON line after reset")
    elif len(boot_ids_after) < 1 or len(set(boot_ids_all)) < 2:
        reasons.append("RESET_REASON boot_id did not change across reset")
    elif boot_ids_after[0] in boot_ids_all[: -len(boot_ids_after)]:
        reasons.append("second RESET_REASON boot_id matches an earlier boot")

    for marker in ("Normal mode: Wi-Fi STA", "Wi-Fi mode: STA"):
        if marker not in after_text:
            reasons.append(f"missing post-reset log marker: {marker}")
    if "Setup AP started" in after_text:
        reasons.append("Setup AP started again after reset")

    status = onb.get("status_after_reset")
    if _http_status(status) != "200":
        reasons.append(
            f"HTTPS /api/status returned {_http_status(status)!r} after reset; expected HTTP 200 within 90s"
        )
    elif not isinstance(status, dict) or not isinstance(status.get("elapsed_seconds"), (int, float)) or status["elapsed_seconds"] > 90:
        reasons.append("HTTPS /api/status did not return HTTP 200 within 90 seconds after reset")

    port80 = onb.get("port80_after_reset")
    if not _port80_is_not_setup(port80):
        reasons.append("port 80 served /setup content or did not provide classifiable non-setup evidence after reset")
    reset_reply = str(onb.get("reset_reply", "")).strip().lower()
    if reset_reply and reset_reply != "ok":
        reasons.append(f"control channel returned {reset_reply!r} for reset")
    return not reasons, reasons


def evaluate_s6(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Check task-core placement for every matching Chrome trace X event."""
    reasons = _common_reasons(log_text, extra)
    values = extra or {}
    if values.get("task_core_error"):
        reasons.append(f"could not derive task core map: {values['task_core_error']}")
    expected = values.get("expected_task_cores")
    if not isinstance(expected, dict) or not expected:
        reasons.append("source-derived task core map is missing")
        expected = {}

    events = values.get("_trace_events")
    if not isinstance(events, list):
        reasons.append("Chrome trace events are missing or invalid")
        return not reasons, reasons

    seen: set[str] = set()
    wrong: dict[str, int] = {}
    for event in events:
        if not isinstance(event, dict) or event.get("ph") != "X":
            continue
        name = event.get("name")
        if name not in expected:
            continue
        seen.add(name)
        args = event.get("args")
        cpu = args.get("cpu") if isinstance(args, dict) else None
        if cpu != expected[name]:
            wrong[name] = wrong.get(name, 0) + 1

    for name in sorted(_REQUIRED_TRACE_TASKS):
        if name not in expected:
            reasons.append(f"source-derived task core map is missing required task {name}")
        elif name not in seen:
            reasons.append(f"required task {name} never appeared in the trace")
    for name, count in sorted(wrong.items()):
        reasons.append(
            f"task {name} had {count} trace event(s) on the wrong core; expected core {expected[name]}"
        )
    return not reasons, reasons


def _post_reset_log(log_text: str) -> str:
    lines = log_text.splitlines()
    reset_positions = [
        index for index, line in enumerate(lines) if _RESET_REASON_LINE.search(line)
    ]
    return "\n".join(lines[reset_positions[1] :]) if len(reset_positions) >= 2 else ""


def evaluate_s8(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Check for watchdog/panic markers across ONB and at least 120s post-reset."""
    reasons = _common_reasons(log_text, extra)
    lowered = log_text.lower()
    for marker in _WATCHDOG_MARKERS:
        if marker.lower() in lowered:
            reasons.append(f"watchdog/stability marker found: {marker}")

    post_reset = _post_reset_log(log_text)
    if not post_reset:
        reasons.append("post-reset log segment is missing")
    else:
        timestamps = [int(value) for value in _LOG_TIMESTAMP.findall(post_reset)]
        if not timestamps:
            reasons.append("post-reset log segment has no emulated timestamps")
        elif max(timestamps) < 120_000:
            reasons.append(
                f"post-reset log spans {max(timestamps)} ms; at least 120000 ms is required"
            )
    return not reasons, reasons


def evaluate_s9(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Check one interrupt-WDT panic, firmware reboot, and successful recovery boot."""
    reasons: list[str] = []
    if extra:
        if extra.get("run_error"):
            reasons.append(f"runner error: {extra['run_error']}")
        return_code = extra.get("wsl_returncode")
        if return_code is not None and return_code != 0:
            reasons.append(f"WSL scenario command exited with {return_code}")

    lines = log_text.splitlines()
    trigger = "TEST_WDT: triggering interrupt watchdog now"
    trigger_indices = [index for index, line in enumerate(lines) if trigger in line]
    if len(trigger_indices) != 1:
        reasons.append(
            f"expected exactly one interrupt-WDT trigger marker, found {len(trigger_indices)}"
        )

    panic_indices = [
        index
        for index, line in enumerate(lines)
        if re.search(r"Guru Meditation Error:", line, re.IGNORECASE)
    ]
    if len(panic_indices) != 1:
        reasons.append(f"expected exactly one panic, found {len(panic_indices)}")
    if trigger_indices and not any(
        index > trigger_indices[0]
        and re.search(r"Guru Meditation Error:.*Interrupt wdt timeout", lines[index], re.IGNORECASE)
        for index in panic_indices
    ):
        reasons.append("missing Interrupt wdt timeout panic after the trigger")
    if trigger_indices and len(panic_indices) == 1 and panic_indices[0] <= trigger_indices[0]:
        reasons.append("interrupt-WDT panic occurred before its trigger marker")

    reset_entries = [
        (index, line)
        for index, line in enumerate(lines)
        if _RESET_REASON_LINE.search(line)
    ]
    reset_reasons = [
        match.group(1)
        for _, line in reset_entries
        if (match := re.search(r"\breason=([A-Z_]+)", line))
    ]
    boot_ids = _RESET_BOOT_ID.findall(log_text)
    if len(reset_entries) != 2:
        reasons.append(f"expected exactly two RESET_REASON lines, found {len(reset_entries)}")
    if reset_entries and trigger_indices and reset_entries[0][0] >= trigger_indices[0]:
        reasons.append("initial POWERON RESET_REASON appeared after the interrupt-WDT trigger")
    if len(reset_reasons) != 2:
        reasons.append("could not read reset reasons from both RESET_REASON lines")
    elif reset_reasons[0] != "POWERON":
        reasons.append(f"first reset reason is {reset_reasons[0]!r}; expected POWERON")
    elif reset_reasons[1] not in {"INT_WDT", "PANIC"}:
        reasons.append(
            f"second reset reason is {reset_reasons[1]!r}; expected INT_WDT or PANIC"
        )
    if len(boot_ids) != 2:
        reasons.append(f"expected two RESET_REASON boot_ids, found {len(boot_ids)}")
    elif boot_ids[0] == boot_ids[1]:
        reasons.append("recovery boot reused the first boot_id")

    bringup_indices = [
        index for index, line in enumerate(lines) if "Hardware bring-up completed" in line
    ]
    if len(bringup_indices) != 2:
        reasons.append(
            f"expected exactly two Hardware bring-up completed markers, found {len(bringup_indices)}"
        )

    if len(reset_entries) == 2:
        second_reset_index = reset_entries[1][0]
        if panic_indices and second_reset_index <= panic_indices[0]:
            reasons.append("recovery RESET_REASON appeared before the interrupt-WDT panic")
        skipped_indices = [
            index
            for index, line in enumerate(lines)
            if "TEST_WDT: skipped" in line
        ]
        if not skipped_indices:
            reasons.append("missing TEST_WDT: skipped marker after the recovery reset")
        else:
            if any(index <= second_reset_index for index in skipped_indices):
                reasons.append("TEST_WDT: skipped appeared before the recovery RESET_REASON")
            if panic_indices:
                panic_index = panic_indices[0]
                if any(index <= panic_index for index in skipped_indices):
                    reasons.append("TEST_WDT: skipped appeared before the interrupt-WDT panic")
                if not any(
                    index > panic_index and index > second_reset_index
                    for index in skipped_indices
                ):
                    reasons.append(
                        "missing TEST_WDT: skipped marker after the interrupt-WDT panic and recovery reset"
                    )
            elif not any(index > second_reset_index for index in skipped_indices):
                reasons.append("missing TEST_WDT: skipped marker after the recovery reset")

            if len(reset_reasons) == 2:
                for index in skipped_indices:
                    match = re.search(
                        r"reset reason is not POWERON\s*\(([^)]+)\)",
                        lines[index],
                        re.IGNORECASE,
                    )
                    if not match:
                        reasons.append("could not read reset reason from TEST_WDT: skipped marker")
                        continue
                    skipped_reason = match.group(1).upper()
                    if skipped_reason == "POWERON":
                        reasons.append(
                            "TEST_WDT: skipped reported POWERON; expected the non-POWERON recovery reason"
                        )
                    elif skipped_reason != reset_reasons[1]:
                        reasons.append(
                            "TEST_WDT: skipped reported "
                            f"{skipped_reason!r}; expected recovery reason {reset_reasons[1]!r}"
                        )
        if len(bringup_indices) >= 2 and second_reset_index >= bringup_indices[1]:
            reasons.append("recovery RESET_REASON appeared after recovery bring-up")

    if len(bringup_indices) >= 2:
        tail_fatal = _panic_lines("\n".join(lines[bringup_indices[1] + 1 :]))
        if tail_fatal:
            reasons.append(f"panic/fatal marker after recovery bring-up: {tail_fatal[0].strip()}")

    return not reasons, reasons


def parse_task_core_macros(source_text: str) -> dict[str, int]:
    """Resolve integer APP_TASK_CORE_* definitions and aliases from the policy header."""
    raw = {name: value.strip() for name, value in _CORE_DEFINE.findall(source_text)}
    resolved: dict[str, int] = {}

    def resolve(name: str, active: set[str]) -> int | None:
        if name in resolved:
            return resolved[name]
        if name in active or name not in raw:
            return None
        value = raw[name]
        while value.startswith("(") and value.endswith(")"):
            value = value[1:-1].strip()
        try:
            result = int(value, 0)
        except ValueError:
            result = resolve(value, active | {name})
        if result is not None:
            resolved[name] = result
        return result

    for name in raw:
        resolve(name, set())
    return resolved


def trace_task_name(task_name: str) -> str:
    """Return the task name used by esp-emu's 15-character trace labels."""
    return task_name[:15]


def expected_trace_task_cores(
    source_text: str, observed_task_names: set[str] | None = None
) -> dict[str, int]:
    """Build the expected trace map, adding optional localization only if seen."""
    macros = parse_task_core_macros(source_text)
    expected: dict[str, int] = {}
    for task_name, macro in _TRACE_CORE_MACROS.items():
        trace_name = trace_task_name(task_name)
        if task_name == "localization_job" and (
            observed_task_names is None or trace_name not in observed_task_names
        ):
            continue
        if macro not in macros:
            raise ValueError(f"missing or unresolved {macro} in app_task_config.h")
        expected[trace_name] = macros[macro]
    return expected


# Stack of the tasks the recording path runs on must never reach the safety margin the firmware itself logs.
_S10_FREE_STACK = re.compile(r"STACK_HWM task=(\S+) stage=(\S+) free=(\d+) margin=(\d+)")
# Where the synthetic source puts the impact (app_test_recording_scenario.h: 500 mm, 3000 mm) and how well it must be found.
S10_SOURCE_M = (0.5, 3.0)
S10_SOURCE_TOLERANCE_M = 0.5
S10_MIN_CLOSE_METHODS = 3
_S10_SYSTEM_LOW_STACK = ("ipc", "IDLE", "Tmr", "esp_timer", "tiT", "wifi", "main", "sys_evt")


def evaluate_s10(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Real START -> record -> STOP with the synthetic source on a flash FAT stand-in for the SD card."""
    reasons = _common_reasons(log_text, extra)
    stages = (
        "TEST_FLASH_STORAGE mounted",
        "TEST_RECORDING: begin",
        "TEST_RECORDING: dma ballast",
        "TEST_RECORDING: psram ballast",
        "TEST_RECORDING: object=OBJ_",
        "TEST_RECORDING: setup seeded",
        "TEST_RECORDING: recording started session=S",
        "TEST_RECORDING: recording samples=",
        "TEST_RECORDING: stopped state=REVIEW",
        "TEST_RECORDING: calculation job=",
        "TEST_RECORDING: DONE",
    )
    positions = []
    for stage in stages:
        index = log_text.find(stage)
        if index < 0:
            reasons.append(f"missing recording marker: {stage}")
        positions.append(index)
    if all(position >= 0 for position in positions) and positions != sorted(positions):
        reasons.append("recording markers are out of order")
    if "TEST_RECORDING: FAIL" in log_text:
        for line in log_text.splitlines():
            if "TEST_RECORDING: FAIL" in line:
                reasons.append(f"recording scenario failed: {line.strip()}")
    for line in log_text.splitlines():
        if "stack overflow" in line.lower():
            reasons.append(f"stack overflow: {line.strip()}")
            break
    recording = re.search(r"TEST_RECORDING: recording samples=(\d+) dropped=(\d+) ms=(\d+)", log_text)
    if recording:
        samples, dropped, millis = (int(value) for value in recording.groups())
        if samples < 250:
            reasons.append(f"recorded only {samples} samples in {millis} ms")
        if dropped != 0:
            reasons.append(f"recorder dropped {dropped} frames")
    stopped = re.search(r"TEST_RECORDING: stopped state=REVIEW samples=(\d+) dropped=(\d+)", log_text)
    if stopped and int(stopped.group(2)) != 0:
        reasons.append(f"recorder dropped {stopped.group(2)} frames by STOP")
    ballast = re.search(r"TEST_RECORDING: dma ballast .*applied=(\d)", log_text)
    if ballast and ballast.group(1) != "1":
        reasons.append("the board-equivalent DMA budget could not be applied")
    for line in log_text.splitlines():
        if "Failed to allocate priv RX buffer" in line:
            reasons.append(f"SD driver found no DMA bounce buffer: {line.strip()}")
            break
    psram = re.search(r"TEST_RECORDING: psram ballast .*applied=(\d)", log_text)
    if psram and psram.group(1) != "1":
        reasons.append("the board-equivalent PSRAM layout could not be applied")
    for line in log_text.splitlines():
        if "insufficient_memory" in line:
            reasons.append(f"the calculation found no memory for its window: {line.strip()}")
            break
    bounce = re.search(r"TEST_RECORDING: sd dma bounce failures=(\d+)", log_text)
    if bounce is None:
        reasons.append("missing recording marker: TEST_RECORDING: sd dma bounce failures=")
    elif int(bounce.group(1)) != 0:
        reasons.append(f"{bounce.group(1)} SD transfers found no DMA bounce buffer")
    # The source calculation on the recorded session (physical setup: components/app_config/include/app_test_recording_scenario.h).
    picks = re.search(r"LOC_PICKS accepted=(\d+) of (\d+) placed", log_text)
    if picks is None:
        reasons.append("the calculation never reached the picks: no LOC_PICKS line (see LOC_SEARCH / LOC_REJECT)")
    elif int(picks.group(1)) < 3:
        reasons.append(f"only {picks.group(1)} of {picks.group(2)} channel picks were accepted")
    for line in log_text.splitlines():
        if "LOC_REJECT" in line:
            reasons.append(f"the calculation rejected the trace: {line.strip()}")
            break
    # The board loses a few AD7606 conversions under load; the synthetic source loses some too and the calculation has
    # to repair exactly those (log 2026-11-14: one lost sample rejected the whole recording as session_gap).
    missed = [int(value) for value in re.findall(r"TEST_SOURCE_HOLE at_frame=\d+ missed=(\d+)", log_text)]
    window = re.search(r"LOC_WINDOW \S+ .*repaired_samples=(\d+)", log_text)
    if not missed:
        reasons.append("the synthetic source lost no conversions: the hole repair was not exercised (no TEST_SOURCE_HOLE)")
    elif window is None:
        reasons.append("no LOC_WINDOW line with repaired_samples")
    elif int(window.group(1)) != sum(missed):
        reasons.append(f"the source lost {sum(missed)} conversions but the calculation repaired {window.group(1)}")
    calculation = re.search(r"TEST_RECORDING: calculation job=\d+ state=(\S+) solved=(\d+)", log_text)
    if calculation and calculation.group(1) != "succeeded":
        reasons.append(f"calculation ended in state {calculation.group(1)}")
    close_methods = []
    for method, x, y, z in re.findall(
        r"LOC_SOLUTION (\S+)\s+pos_m=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", log_text
    ):
        error_m = ((float(x) - S10_SOURCE_M[0]) ** 2 + (float(y) - S10_SOURCE_M[1]) ** 2) ** 0.5
        if error_m <= S10_SOURCE_TOLERANCE_M:
            close_methods.append(method)
    if len(close_methods) < S10_MIN_CLOSE_METHODS:
        reasons.append(
            f"only {len(close_methods)} method(s) located the source within {S10_SOURCE_TOLERANCE_M} m of "
            f"{S10_SOURCE_M}: {close_methods}"
        )
    hwm_lines = 0
    for task, stage, free, margin in _S10_FREE_STACK.findall(log_text):
        hwm_lines += 1
        if int(free) < int(margin):
            reasons.append(f"{task} stack below its safety margin at {stage}: free={free} margin={margin}")
    if hwm_lines == 0:
        reasons.append("no STACK_HWM line: the measurement path did not report its stack")
    begin = log_text.find("TEST_RECORDING: begin")
    if begin >= 0:
        for line in log_text[begin:].splitlines():
            low = re.search(r"RUNTIME_TASK_LOW_STACK name=(\S+) stack_free=(\d+)", line)
            if low and not low.group(1).startswith(_S10_SYSTEM_LOW_STACK):
                reasons.append(f"task {low.group(1)} ran low on stack while recording: {low.group(2)} bytes free")
    return not reasons, reasons


EVALUATORS = {
    "S1": evaluate_s1,
    "S2": evaluate_s2,
    "S3": evaluate_s3,
    "S4": evaluate_s4,
    "S5": evaluate_s5,
    "S6": evaluate_s6,
    "S7": evaluate_s7,
    "S8": evaluate_s8,
    "S9": evaluate_s9,
    "S10": evaluate_s10,
}


def merge_flash_image(
    bootloader: bytes,
    partition_table: bytes,
    app: bytes,
    flash_size: int = FLASH_SIZE,
) -> bytes:
    """Create a raw 16 MB flash image with ESP-IDF's standard emulator offsets."""
    if flash_size <= 0:
        raise ValueError("flash_size must be positive")
    image = bytearray(b"\xff" * flash_size)
    occupied: list[tuple[int, int, str]] = []
    for name, payload in (
        ("bootloader", bootloader),
        ("partition_table", partition_table),
        ("app", app),
    ):
        offset = FLASH_OFFSETS[name]
        end = offset + len(payload)
        if end > flash_size:
            raise ValueError(
                f"{name} at 0x{offset:x} exceeds the {flash_size}-byte flash image"
            )
        for previous_start, previous_end, previous_name in occupied:
            if offset < previous_end and previous_start < end:
                raise ValueError(f"{name} overlaps {previous_name}")
        if payload:
            image[offset:end] = payload
            occupied.append((offset, end, name))
    return bytes(image)


def build_merged_image(build_dir: Path, image_path: Path) -> None:
    """Merge the three files and offsets declared by ESP-IDF's flasher_args.json."""
    build_dir = build_dir.resolve()
    flasher_args_path = build_dir / "flasher_args.json"
    flasher = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    flash_files = flasher.get("flash_files", {})
    by_offset = {int(offset, 0): filename for offset, filename in flash_files.items()}
    expected = {
        FLASH_OFFSETS["bootloader"],
        FLASH_OFFSETS["partition_table"],
        FLASH_OFFSETS["app"],
    }
    if not expected.issubset(by_offset):
        raise ValueError("flasher_args.json is missing bootloader, partition table, or app")
    declared_size = flasher.get("flash_settings", {}).get("flash_size", "16MB")
    if str(declared_size).upper() != "16MB":
        raise ValueError(f"expected a 16MB firmware image, found {declared_size!r}")

    def read_component(offset: int) -> bytes:
        component_path = (build_dir / by_offset[offset]).resolve()
        try:
            component_path.relative_to(build_dir)
        except ValueError as exc:
            raise ValueError(f"flash component escapes build directory: {component_path}") from exc
        return component_path.read_bytes()

    image_bytes = merge_flash_image(
        read_component(FLASH_OFFSETS["bootloader"]),
        read_component(FLASH_OFFSETS["partition_table"]),
        read_component(FLASH_OFFSETS["app"]),
    )
    image_path.parent.mkdir(parents=True, exist_ok=True)
    image_path.write_bytes(image_bytes)


def to_wsl_path(path: str | Path) -> str:
    """Convert an absolute Windows drive path to its mounted WSL path."""
    windows_path = PureWindowsPath(str(path))
    drive = windows_path.drive
    if not windows_path.is_absolute() or len(drive) != 2 or not drive.endswith(":"):
        raise ValueError(f"expected an absolute drive path, got {path!r}")
    parts = windows_path.parts[1:]
    suffix = "/".join(parts)
    return f"/mnt/{drive[0].lower()}" + (f"/{suffix}" if suffix else "")


def shell_join(arguments: list[str]) -> str:
    """Quote an argv list for a single Bash command string."""
    return shlex.join([str(argument) for argument in arguments])


def build_emulator_argv(
    scenario: str,
    emu: str,
    image: str,
    elf: str,
) -> tuple[list[str], int, int]:
    """Return emulator argv, scenario timeout, and outer timeout in seconds."""
    timeout = {"S1": 70, "S2": 70, "S3": 40, "S4": 100, "S9": 100, "S10": 300}[scenario]
    outer_timeout = timeout + (10 if scenario != "S4" else 15)
    argv = [
        emu,
        "--chip",
        "esp32s3",
        "--firmware",
        image,
        "--elf",
        elf,
        "--timeout",
        f"{timeout}s",
    ]
    if scenario == "S9":
        argv.append("--no-panic-intercept")
    elif scenario == "S2":
        argv.extend(("--control-tcp", "127.0.0.1:5556"))
    elif scenario == "S3":
        argv.extend(("--psram-size", "4M"))
    elif scenario == "S4":
        argv.extend(
            (
                "--wifi-fw-ap-password",
                "GEOLOCATOR",
                "--net",
                "user,hostfwd=tcp::18080-:80",
            )
        )
    return argv, timeout, outer_timeout


def build_wsl_argv(script_path: str, distro: str = "Ubuntu") -> list[str]:
    """Build the Windows argv used to run a script file inside WSL."""
    return ["wsl.exe", "-d", distro, "-u", "root", "--", "bash", script_path]


def build_scenario_script(
    scenario: str,
    repo_wsl: str,
    emu: str,
    image_wsl: str,
    elf_wsl: str,
    log_wsl: str,
    out_wsl: str,
) -> str:
    argv, _, outer_timeout = build_emulator_argv(
        scenario, emu, image_wsl, elf_wsl
    )
    duration_wsl = f"{out_wsl.rstrip('/')}/{scenario.lower()}.duration"
    command = (
        f"timeout --signal=TERM --kill-after=5s {outer_timeout}s "
        f"{shell_join(argv)} > {shlex.quote(log_wsl)} 2>&1"
    )
    duration_write = (
        "end_s=$(date +%s)\n"
        + f"printf 'EMU_DURATION_SECONDS=%s\\n' \"$((end_s - start_s))\" > {shlex.quote(duration_wsl)}\n"
    )
    common = (
        "#!/usr/bin/env bash\nset -u\n"
        + f"cd -- {shlex.quote(repo_wsl)}\nmkdir -p -- {shlex.quote(out_wsl)}\n"
    )
    if scenario == "S2":
        return (
            common
            + "start_s=$(date +%s)\n"
            + f"{command} &\nEP=$!\n"
            + "cleanup() { if kill -0 \"$EP\" 2>/dev/null; then "
            + "kill -TERM \"$EP\" 2>/dev/null || true; fi; }\n"
            + "trap cleanup EXIT INT TERM\n"
            + "sleep 30\n"
            + f"EMU_LOG={shlex.quote(log_wsl)} python3 - <<'PY'\n"
            + "import json, os, re, socket\n"
            + "def read_reply(sock):\n"
            + "    data = bytearray()\n"
            + "    while b'\\n' not in data and b'ok' not in data.lower():\n"
            + "        try:\n"
            + "            chunk = sock.recv(200)\n"
            + "        except socket.timeout:\n"
            + "            break\n"
            + "        if not chunk:\n"
            + "            break\n"
            + "        data.extend(chunk)\n"
            + "    return data.decode('utf-8', 'replace').strip()\n"
            + "def boot_ids():\n"
            + "    try:\n"
            + "        with open(os.environ['EMU_LOG'], encoding='utf-8', errors='replace') as log_file:\n"
            + "            return re.findall(r'\\bRESET_REASON\\b[^\\r\\n]*\\bboot_id=([0-9A-Fa-fx]+)', log_file.read(), re.IGNORECASE)\n"
            + "    except OSError:\n"
            + "        return []\n"
            + "s = socket.create_connection(('127.0.0.1', 5556), timeout=5)\n"
            + "s.settimeout(5)\n"
            + "s.sendall(b'ping\\n')\n"
            + "ping = read_reply(s)\n"
            + "initial_boot_ids = set(boot_ids())\n"
            + "s.sendall(b'reset\\n')\n"
            + "reset = read_reply(s)\n"
            + "s.close()\n"
            + "current_boot_ids = boot_ids()\n"
            + "second_boot_observed = (len(current_boot_ids) >= 2 and len(set(current_boot_ids)) >= 2 and bool(set(current_boot_ids) - initial_boot_ids))\n"
            + "if reset == '' and not second_boot_observed:\n"
            + "    retry = socket.create_connection(('127.0.0.1', 5556), timeout=5)\n"
            + "    retry.settimeout(5)\n"
            + "    retry.sendall(b'reset\\n')\n"
            + "    reset = read_reply(retry)\n"
            + "    retry.close()\n"
            + "print('CONTROL_RESULTS=' + json.dumps({'ping': ping, 'reset': reset}, separators=(',', ':')))\n"
            + "PY\ncontrol_rc=$?\n"
            + "wait \"$EP\"\nemu_rc=$?\ntrap - EXIT INT TERM\n"
            + duration_write
            + "if [ \"$control_rc\" -ne 0 ]; then exit \"$control_rc\"; fi\n"
            + "exit \"$emu_rc\"\n"
        )
    if scenario == "S4":
        return (
            common
            + "start_s=$(date +%s)\n"
            + f"{command} &\nEP=$!\n"
            + "cleanup() { if kill -0 \"$EP\" 2>/dev/null; then "
            + "kill -TERM \"$EP\" 2>/dev/null || true; fi; }\n"
            + "trap cleanup EXIT INT TERM\n"
            + "sleep 60\n"
            + "python3 - <<'PY'\n"
            + "import json, urllib.error, urllib.request\n"
            + "class NoRedirect(urllib.request.HTTPRedirectHandler):\n"
            + "    def redirect_request(self, req, fp, code, msg, headers, newurl):\n"
            + "        return None\n"
            + "opener = urllib.request.build_opener(NoRedirect)\n"
            + "def get(path):\n"
            + "    try:\n"
            + "        response = opener.open('http://127.0.0.1:18080' + path, timeout=8)\n"
            + "        return {'status': response.status, 'body': response.read().decode('utf-8', 'replace')}\n"
            + "    except urllib.error.HTTPError as error:\n"
            + "        return {'status': error.code, 'body': error.read().decode('utf-8', 'replace')}\n"
            + "    except Exception as error:\n"
            + "        return {'status': None, 'body': type(error).__name__ + ': ' + str(error)}\n"
            + "results = {path: get(path) for path in ('/setup', '/', '/api/status')}\n"
            + "print('HTTP_RESULTS=' + json.dumps(results, separators=(',', ':')))\n"
            + "PY\nprobe_rc=$?\n"
            + "wait \"$EP\"\nemu_rc=$?\ntrap - EXIT INT TERM\n"
            + duration_write
            + "if [ \"$probe_rc\" -ne 0 ]; then exit \"$probe_rc\"; fi\n"
            + "exit \"$emu_rc\"\n"
        )
    return (
        common
        + "start_s=$(date +%s)\n"
        + f"{command}\nemu_rc=$?\n"
        + duration_write
        + "exit \"$emu_rc\"\n"
    )


def build_onb_emulator_argv(
    emu: str,
    image_wsl: str,
    elf_wsl: str,
    trace_wsl: str,
) -> tuple[list[str], int, int]:
    """Return the single ONB emulator command and its guest/host timeouts."""
    argv = [
        emu,
        "--chip",
        "esp32s3",
        "--firmware",
        image_wsl,
        "--elf",
        elf_wsl,
        "--wifi-fw-ap-password",
        "GEOLOCATOR",
        "--net",
        "user,hostfwd=tcp::18080-:80,hostfwd=tcp::18443-:443",
        "--control-tcp",
        "127.0.0.1:5556",
        "--trace",
        trace_wsl,
        "--timeout",
        f"{ONB_TIMEOUT_SECONDS}s",
    ]
    return argv, ONB_TIMEOUT_SECONDS, ONB_OUTER_TIMEOUT_SECONDS


def build_onb_script(
    repo_wsl: str,
    emu: str,
    image_wsl: str,
    elf_wsl: str,
    log_wsl: str,
    trace_wsl: str,
    out_wsl: str,
) -> str:
    """Build the LF-only WSL script that drives S5-S8 on one emulator boot."""
    argv, _, outer_timeout = build_onb_emulator_argv(
        emu, image_wsl, elf_wsl, trace_wsl
    )
    command = (
        f"timeout --signal=TERM --kill-after=5s {outer_timeout}s "
        f"{shell_join(argv)} > {shlex.quote(log_wsl)} 2>&1"
    )
    net_argument = "user,hostfwd=tcp::18080-:80,hostfwd=tcp::18443-:443"
    command = command.replace(
        f"--net {net_argument}", f"--net '{net_argument}'", 1
    )
    common = (
        "#!/usr/bin/env bash\nset -u\n"
        + f"cd -- {shlex.quote(repo_wsl)}\nmkdir -p -- {shlex.quote(out_wsl)}\n"
    )
    client = r'''import json
import os
import socket
import subprocess
import time
import urllib.error
import urllib.request

root = os.environ["ONB_DIR"]
log_path = os.environ["EMU_LOG"]
ca_path = os.path.join(root, "ca.crt")
http_root = "http://127.0.0.1:18080"
tls_root = "https://geolocator.local:18443"
resolve = "geolocator.local:18443:127.0.0.1"
results = {"http": {}}

def curl(args, timeout_seconds=10):
    try:
        proc = subprocess.run(
            ["curl", "-sS", "-m", str(timeout_seconds), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds + 2,
            check=False,
        )
        parts = proc.stdout.strip().split()
        status = parts[0] if parts and len(parts[0]) == 3 else "000"
        result = {"status": status}
        if len(parts) > 1:
            try:
                result["bytes"] = int(float(parts[1]))
            except ValueError:
                pass
        if proc.stderr.strip():
            result["error"] = proc.stderr.strip()[-500:]
        return result
    except Exception as error:
        return {"status": "000", "error": type(error).__name__ + ": " + str(error)}

def get_json_file(path):
    try:
        with open(path, encoding="utf-8") as source:
            value = json.load(source)
        return value if isinstance(value, dict) else None
    except (OSError, json.JSONDecodeError):
        return None

def read_ca():
    try:
        with open(ca_path, "rb") as source:
            data = source.read()
        return data
    except OSError:
        return b""

def poll_status(timeout_seconds, started_at=None, label="status"):
    start = time.monotonic() if started_at is None else started_at
    deadline = start + timeout_seconds
    attempts = 0
    latest = {"status": "000"}
    status_path = os.path.join(root, label + ".json")
    while time.monotonic() < deadline:
        attempts += 1
        try:
            os.unlink(status_path)
        except OSError:
            pass
        latest = curl(
            [
                "--cacert", ca_path,
                "--resolve", resolve,
                "-o", status_path,
                "-w", "%{http_code}",
                tls_root + "/api/status",
            ],
            timeout_seconds=10,
        )
        if latest.get("status") == "200":
            latest["json"] = get_json_file(status_path)
            break
        if time.monotonic() < deadline:
            time.sleep(min(2, max(0, deadline - time.monotonic())))
    latest["attempts"] = attempts
    latest["elapsed_seconds"] = round(time.monotonic() - start, 3)
    return latest

def read_reply(sock):
    data = bytearray()
    while b"\n" not in data and b"ok" not in data.lower():
        try:
            chunk = sock.recv(200)
        except socket.timeout:
            break
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return data.decode("utf-8", "replace").strip()

def port80_probe():
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    opener = urllib.request.build_opener(NoRedirect)
    try:
        response = opener.open(http_root + "/setup", timeout=5)
        return {
            "status": response.status,
            "body": response.read().decode("utf-8", "replace"),
            "location": response.headers.get("Location", ""),
        }
    except urllib.error.HTTPError as error:
        return {
            "status": error.code,
            "body": error.read().decode("utf-8", "replace"),
            "location": error.headers.get("Location", ""),
        }
    except urllib.error.URLError as error:
        return {"status": None, "error": str(error.reason)}
    except Exception as error:
        return {"status": None, "error": type(error).__name__ + ": " + str(error)}

try:
    results["http"]["/setup/ca-guide"] = curl(
        ["-o", os.devnull, "-w", "%{http_code}", http_root + "/setup/ca-guide"]
    )
    ca_download = curl(
        ["-o", ca_path, "-w", "%{http_code} %{size_download}", http_root + "/setup/ca-download"]
    )
    ca_data = read_ca()
    ca_download["bytes"] = len(ca_data)
    ca_download["pem"] = ca_data.startswith(b"-----BEGIN CERTIFICATE-----")
    results["http"]["/setup/ca-download"] = ca_download
    results["http"]["/setup/ca?installed=1"] = curl(
        ["-o", os.devnull, "-w", "%{http_code}", http_root + "/setup/ca?installed=1"]
    )
    results["http"]["/setup"] = curl(
        ["-o", os.devnull, "-w", "%{http_code}", http_root + "/setup"]
    )
    results["http"]["POST /api/wifi/setup"] = curl(
        [
            "-o", os.devnull, "-w", "%{http_code}",
            "--data-urlencode", "ssid=myssid",
            "--data-urlencode", "password=mypassword",
            http_root + "/api/wifi/setup",
        ],
        timeout_seconds=15,
    )
    status_started = time.monotonic()
    results["status_before_reset"] = poll_status(
        60, started_at=status_started, label="status-before-reset"
    )

    control_path = os.path.join(root, "acquisition-start.json")
    acquisition = curl(
        [
            "--cacert", ca_path, "--resolve", resolve,
            "-X", "POST", "-d", '{"action":"acquisition_start"}',
            "-H", "Content-Type: application/json",
            "-o", control_path, "-w", "%{http_code}",
            tls_root + "/api/control",
        ],
        timeout_seconds=15,
    )
    acquisition["json"] = get_json_file(control_path)
    results["acquisition_start"] = acquisition
    time.sleep(10)
    results["status_after_acquisition_wait"] = curl(
        [
            "--cacert", ca_path, "--resolve", resolve,
            "-o", os.devnull, "-w", "%{http_code}",
            tls_root + "/api/status",
        ]
    )

    try:
        with open(log_path, "rb") as log_file:
            results["reset_log_line"] = log_file.read().count(b"\n")
    except OSError:
        results["reset_log_line"] = None
    reset_started = time.monotonic()
    results["reset_sent"] = False
    try:
        control = socket.create_connection(("127.0.0.1", 5556), timeout=5)
        control.settimeout(5)
        control.sendall(b"reset\n")
        results["reset_sent"] = True
        results["reset_reply"] = read_reply(control)
        control.close()
    except Exception as error:
        results["reset_reply"] = ""
        results["reset_error"] = type(error).__name__ + ": " + str(error)
    results["status_after_reset"] = poll_status(
        90, started_at=reset_started, label="status-after-reset"
    )
    results["port80_after_reset"] = port80_probe()
except Exception as error:
    results["client_error"] = type(error).__name__ + ": " + str(error)

print("ONB_RESULTS=" + json.dumps(results, separators=(",", ":")))
'''
    return (
        common
        + "start_s=$(date +%s)\n"
        + f"{command} &\nEP=$!\n"
        + "cleanup() { if kill -0 \"$EP\" 2>/dev/null; then "
        + "kill -TERM \"$EP\" 2>/dev/null || true; fi; }\n"
        + "trap cleanup EXIT INT TERM\n"
        + "sleep 45\n"
        + f"ONB_DIR={shlex.quote(out_wsl)} EMU_LOG={shlex.quote(log_wsl)} python3 - <<'PY'\n"
        + client
        + "\nPY\nclient_rc=$?\n"
        + "wait \"$EP\"\nemu_rc=$?\ntrap - EXIT INT TERM\n"
        + "end_s=$(date +%s)\n"
        + f"printf 'EMU_DURATION_SECONDS=%s\\n' \"$((end_s - start_s))\" > {shlex.quote(out_wsl.rstrip('/') + '/onb.duration')}\n"
        + "if [ \"$client_rc\" -ne 0 ]; then exit \"$client_rc\"; fi\n"
        + "exit \"$emu_rc\"\n"
    )


def parse_scenarios(value: str) -> list[str]:
    if value.strip().lower() == "all":
        return list(SCENARIOS)
    scenarios = [item.strip().upper() for item in value.split(",") if item.strip()]
    invalid = [item for item in scenarios if item not in EVALUATORS]
    if not scenarios or invalid:
        raise ValueError(f"unknown scenario selection: {value!r}")
    if len(set(scenarios)) != len(scenarios):
        raise ValueError("scenario selection contains duplicates")
    return scenarios


def _json_marker(output: str, prefix: str) -> dict[str, Any] | None:
    for line in output.splitlines():
        if line.startswith(prefix):
            try:
                value = json.loads(line[len(prefix) :])
            except json.JSONDecodeError:
                return None
            return value if isinstance(value, dict) else None
    return None


def _run_scenario(
    scenario: str,
    *,
    emu: str,
    image: Path,
    elf: Path,
    out_dir: Path,
) -> tuple[str, dict[str, Any]]:
    log_path = out_dir / f"{scenario.lower()}.log"
    script_path = out_dir / f"{scenario.lower()}.sh"
    repo_wsl = to_wsl_path(REPO_ROOT)
    out_wsl = to_wsl_path(out_dir.resolve())
    script = build_scenario_script(
        scenario,
        repo_wsl,
        emu,
        to_wsl_path(image.resolve()),
        to_wsl_path(elf.resolve()),
        to_wsl_path(log_path.resolve()),
        out_wsl,
    )
    script_path.write_bytes(script.encode("utf-8"))
    _, _, outer_seconds = build_emulator_argv(
        scenario, emu, to_wsl_path(image.resolve()), to_wsl_path(elf.resolve())
    )
    duration_path = out_dir / f"{scenario.lower()}.duration"
    duration_path.write_text("", encoding="utf-8")
    started = time.monotonic()
    try:
        result = subprocess.run(
            build_wsl_argv(to_wsl_path(script_path.resolve())),
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=outer_seconds + 20,
            check=False,
        )
        elapsed = time.monotonic() - started
        extra: dict[str, Any] = {
            "host_wall_seconds": round(elapsed, 3),
            "wsl_returncode": result.returncode,
        }
        if result.stdout.strip():
            extra["wsl_stdout"] = result.stdout.strip()[-4000:]
        if result.stderr.strip():
            extra["wsl_stderr"] = result.stderr.strip()[-2000:]
        if scenario == "S2":
            extra["control"] = _json_marker(result.stdout, "CONTROL_RESULTS=")
        elif scenario == "S4":
            extra["http"] = _json_marker(result.stdout, "HTTP_RESULTS=")
    except (OSError, subprocess.TimeoutExpired) as error:
        elapsed = time.monotonic() - started
        extra = {"host_wall_seconds": round(elapsed, 3), "run_error": str(error)}
    try:
        duration_text = duration_path.read_text(encoding="utf-8").strip()
        duration_match = re.search(r"EMU_DURATION_SECONDS=([0-9]+(?:\.[0-9]+)?)", duration_text)
        if duration_match:
            extra["duration_seconds"] = float(duration_match.group(1))
        else:
            extra.setdefault("run_error", "WSL did not record emulator runtime")
    except OSError:
        extra.setdefault("run_error", "WSL did not record emulator runtime")
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        log_text = ""
    return log_text, extra


def _run_onb(
    *,
    emu: str,
    image: Path,
    elf: Path,
    out_dir: Path,
) -> tuple[str, dict[str, Any]]:
    """Run S5-S8 as one ONB emulator process and collect shared evidence."""
    onb_dir = out_dir / "onb"
    onb_dir.mkdir(parents=True, exist_ok=True)
    log_path = onb_dir / "emu.log"
    trace_path = onb_dir / "trace.json"
    script_path = onb_dir / "onb.sh"
    duration_path = onb_dir / "onb.duration"
    repo_wsl = to_wsl_path(REPO_ROOT)
    onb_wsl = to_wsl_path(onb_dir.resolve())
    script = build_onb_script(
        repo_wsl,
        emu,
        to_wsl_path(image.resolve()),
        to_wsl_path(elf.resolve()),
        to_wsl_path(log_path.resolve()),
        to_wsl_path(trace_path.resolve()),
        onb_wsl,
    )
    script_path.write_bytes(script.encode("utf-8"))
    duration_path.write_text("", encoding="utf-8")
    started = time.monotonic()
    try:
        result = subprocess.run(
            build_wsl_argv(to_wsl_path(script_path.resolve())),
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=ONB_OUTER_TIMEOUT_SECONDS + 70,
            check=False,
        )
        extra: dict[str, Any] = {
            "host_wall_seconds": round(time.monotonic() - started, 3),
            "wsl_returncode": result.returncode,
        }
        if result.stderr.strip():
            extra["wsl_stderr"] = result.stderr.strip()[-2000:]
        onb_result = _json_marker(result.stdout, "ONB_RESULTS=")
        if onb_result is None:
            onb_result = {"client_error": "WSL did not emit ONB_RESULTS"}
        extra["onb"] = onb_result
    except (OSError, subprocess.TimeoutExpired) as error:
        extra = {
            "host_wall_seconds": round(time.monotonic() - started, 3),
            "run_error": str(error),
            "onb": {"client_error": "ONB WSL runner did not complete"},
        }
    try:
        duration_text = duration_path.read_text(encoding="utf-8").strip()
        duration_match = re.search(
            r"EMU_DURATION_SECONDS=([0-9]+(?:\.[0-9]+)?)", duration_text
        )
        if duration_match:
            extra["duration_seconds"] = float(duration_match.group(1))
        else:
            extra.setdefault("run_error", "WSL did not record ONB emulator runtime")
    except OSError:
        extra.setdefault("run_error", "WSL did not record ONB emulator runtime")

    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        log_text = ""
    try:
        trace_events = json.loads(trace_path.read_text(encoding="utf-8"))
        if not isinstance(trace_events, list):
            raise ValueError("trace root is not a JSON event list")
        extra["_trace_events"] = trace_events
        extra["trace_path"] = str(trace_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        extra["trace_error"] = str(error)
        extra["_trace_events"] = None
    try:
        task_config = (
            REPO_ROOT / "components" / "app_config" / "include" / "app_task_config.h"
        ).read_text(encoding="utf-8")
        trace_events = extra.get("_trace_events")
        observed_names = set()
        if isinstance(trace_events, list):
            observed_names = {
                event.get("name")
                for event in trace_events
                if isinstance(event, dict) and event.get("ph") == "X"
            }
        extra["expected_task_cores"] = expected_trace_task_cores(
            task_config, observed_names
        )
    except (OSError, ValueError) as error:
        extra["task_core_error"] = str(error)
        extra["expected_task_cores"] = {}
    return log_text, extra


def _resolve_path(value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else REPO_ROOT / path


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario",
        default="all",
        metavar="S1,S2,...|all",
        help="comma-separated scenarios to run (default: all)",
    )
    parser.add_argument("--emu", default=DEFAULT_EMU, help="esp-emu path inside WSL")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="merged firmware image")
    parser.add_argument("--elf", default=DEFAULT_ELF, help="emulator-profile ELF")
    parser.add_argument("--wdt-image", default=DEFAULT_WDT_IMAGE, help="merged emulator-wdt firmware image")
    parser.add_argument("--wdt-elf", default=DEFAULT_WDT_ELF, help="emulator-wdt firmware ELF")
    parser.add_argument("--rec-image", default=DEFAULT_REC_IMAGE, help="merged emulator-rec firmware image (S10)")
    parser.add_argument("--rec-elf", default=DEFAULT_REC_ELF, help="emulator-rec firmware ELF (S10)")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help="logs and JSON report directory")
    parser.add_argument(
        "--build-image",
        action="store_true",
        help="merge the 16 MB flash image from .test-build/idf-emulator",
    )
    parser.add_argument(
        "--build-rec-image",
        action="store_true",
        help="merge the 16 MB flash image from .test-build/idf-emulator-rec (S10)",
    )
    parser.add_argument(
        "--build-wdt-image",
        action="store_true",
        help="merge the 16 MB flash image from .test-build/idf-emulator-wdt",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        scenarios = parse_scenarios(args.scenario)
    except ValueError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 2

    all_requested = args.scenario.strip().lower() == "all"
    image = _resolve_path(args.image).resolve()
    elf = _resolve_path(args.elf).resolve()
    wdt_image = _resolve_path(args.wdt_image).resolve()
    wdt_elf = _resolve_path(args.wdt_elf).resolve()
    rec_image = _resolve_path(args.rec_image).resolve()
    rec_elf = _resolve_path(args.rec_elf).resolve()
    out_dir = _resolve_path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    normal_setup_error = None
    if any(scenario not in ("S9", "S10") for scenario in scenarios) and args.build_image:
        try:
            build_merged_image(REPO_ROOT / ".test-build" / "idf-emulator", image)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            normal_setup_error = f"could not build merged image: {error}"
    if any(scenario not in ("S9", "S10") for scenario in scenarios):
        if not args.build_image and not image.is_file():
            normal_setup_error = f"firmware image does not exist: {image}"
        if not elf.is_file():
            normal_setup_error = normal_setup_error or f"firmware ELF does not exist: {elf}"

    rec_setup_error = None
    if "S10" in scenarios:
        if args.build_rec_image:
            try:
                build_merged_image(REPO_ROOT / DEFAULT_REC_BUILD_DIR, rec_image)
            except (OSError, ValueError, json.JSONDecodeError) as error:
                rec_setup_error = f"could not build recording image: {error}"
        if not rec_image.is_file():
            rec_setup_error = rec_setup_error or f"recording image missing: {rec_image}"
        if not rec_elf.is_file():
            rec_setup_error = rec_setup_error or f"recording firmware ELF does not exist: {rec_elf}"

    wdt_build_error = None
    if args.build_wdt_image:
        try:
            build_merged_image(REPO_ROOT / DEFAULT_WDT_BUILD_DIR, wdt_image)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            wdt_build_error = f"could not build watchdog image: {error}"
    wdt_image_available = wdt_image.is_file() and not wdt_build_error
    s9_skip_reason = None
    if all_requested and wdt_image_available:
        scenarios.append("S9")
    elif all_requested:
        s9_skip_reason = f"watchdog image missing: {wdt_image}"
        if wdt_build_error:
            s9_skip_reason += f" ({wdt_build_error})"

    wdt_setup_error = wdt_build_error
    if "S9" in scenarios:
        if not wdt_image.is_file():
            wdt_setup_error = wdt_setup_error or f"watchdog image missing: {wdt_image}"
        if not wdt_elf.is_file():
            wdt_setup_error = wdt_setup_error or f"watchdog firmware ELF does not exist: {wdt_elf}"

    report: dict[str, Any] = {
        "scenarios": {},
        "image": str(image),
        "elf": str(elf),
        "wdt_image": str(wdt_image),
        "wdt_elf": str(wdt_elf),
        "rec_image": str(rec_image),
        "rec_elf": str(rec_elf),
        "limitations": LIMITATIONS,
    }
    all_passed = True
    onb_run: tuple[str, dict[str, Any]] | None = None
    for scenario in scenarios:
        if scenario in ONB_SCENARIOS:
            if onb_run is None:
                if normal_setup_error:
                    onb_run = (
                        "",
                        {
                            "run_error": normal_setup_error,
                            "onb": {"client_error": "firmware image or ELF is unavailable"},
                            "_trace_events": None,
                        },
                    )
                else:
                    onb_run = _run_onb(
                        emu=args.emu, image=image, elf=elf, out_dir=out_dir
                    )
            log_text, extra = onb_run
        elif scenario == "S10":
            if rec_setup_error:
                log_text, extra = "", {"run_error": rec_setup_error}
            else:
                log_text, extra = _run_scenario(
                    scenario, emu=args.emu, image=rec_image, elf=rec_elf, out_dir=out_dir
                )
        elif scenario == "S9":
            if wdt_setup_error:
                log_text, extra = "", {"run_error": wdt_setup_error}
            else:
                log_text, extra = _run_scenario(
                    scenario, emu=args.emu, image=wdt_image, elf=wdt_elf, out_dir=out_dir
                )
        elif normal_setup_error:
            log_text, extra = "", {"run_error": normal_setup_error}
        else:
            log_text, extra = _run_scenario(
                scenario, emu=args.emu, image=image, elf=elf, out_dir=out_dir
            )
        ok, reasons = EVALUATORS[scenario](log_text, extra)
        all_passed = all_passed and ok
        log_path = (
            out_dir / "onb" / "emu.log"
            if scenario in ONB_SCENARIOS
            else out_dir / f"{scenario.lower()}.log"
        )
        report_extra = {key: value for key, value in extra.items() if key != "_trace_events"}
        report["scenarios"][scenario] = {
            "result": "PASS" if ok else "FAIL",
            "reasons": reasons,
            "log": str(log_path),
            "extra": report_extra,
        }
        print(f"{scenario}: {'PASS' if ok else 'FAIL'}")
        for reason in reasons:
            print(f"  - {reason}")

    if s9_skip_reason:
        report["scenarios"]["S9"] = {
            "result": "NOT RUN",
            "reasons": [s9_skip_reason],
            "log": str(out_dir / "s9.log"),
        }
        print("S9: NOT RUN (image missing)")
        if wdt_build_error:
            print(f"  - {wdt_build_error}")

    report_path = out_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")
    print(f"Limits: {LIMITATIONS}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
