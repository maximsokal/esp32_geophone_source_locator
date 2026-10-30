"""Structural guard for the safe-shutdown sequence in main/app_shutdown.c.

The policy pieces (drain with a bound, never keep the power on) are tested on the host in
tests/host/app_shutdown_drain_test.c. This file pins how app_shutdown_wait_and_run() uses them,
because that function cannot be built on the host.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/app_shutdown.c").read_text(encoding="utf-8")


def _function_body(name: str) -> str:
    start = SOURCE.index(f"void {name}(void)")
    open_brace = SOURCE.index("{", start)
    depth = 0
    for index in range(open_brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[open_brace:index + 1]
    raise AssertionError("unbalanced braces")


BODY = _function_body("app_shutdown_wait_and_run")


def _position(text: str, needle: str) -> int:
    index = text.find(needle)
    assert index >= 0, f"{needle!r} is missing from app_shutdown_wait_and_run()"
    return index


def test_writes_are_drained_before_the_network_and_the_services_go_away():
    drain = _position(BODY, "app_shutdown_drain_writes(")
    # The Web UI keeps telling the user "critical battery - stopping" until the writes are done.
    assert drain < _position(BODY, "app_shutdown_stop_network();")
    assert drain < _position(BODY, "measurement_service_shutdown()")
    assert drain < _position(BODY, "ui_shutdown();")
    assert drain < _position(BODY, "storage_service_shutdown();")
    assert drain < _position(BODY, "app_shutdown_stop_storage();")


def test_drain_asks_the_recording_to_stop_and_looks_at_every_sd_writer():
    assert "measurement_service_stop_async()" in SOURCE
    for probe in (
        "measurement_service_get_snapshot(",
        "seismic_recorder_get_snapshot(",
        "object_service_is_busy()",
        "storage_service_is_busy()",
    ):
        assert probe in SOURCE, f"the drain does not look at {probe}"
    for state in ("MEASUREMENT_SERVICE_STARTING", "MEASUREMENT_SERVICE_RECORDING",
                  "MEASUREMENT_SERVICE_STOPPING", "MEASUREMENT_SERVICE_DELETING"):
        assert state in SOURCE


def test_a_failed_measurement_shutdown_never_keeps_the_power_on():
    after_failure = BODY[_position(BODY, "measurement_service_shutdown()"):]
    plan = _position(after_failure, "app_shutdown_plan_after_measurement(")
    tail = after_failure[plan:]
    # No bare early return between the measurement shutdown and the end of the sequence ...
    assert not re.search(r"\breturn\s*;", after_failure)
    # ... and the emergency branch cuts the power itself.
    assert tail.count("power_control_shutdown();") >= 2
    assert "!plan.cut_power" not in tail  # the plan always cuts power; the code does not second-guess it


def test_the_drain_is_bounded_by_the_recording_stop_limit():
    drain = BODY[_position(BODY, "app_shutdown_drain_writes("):]
    call = drain[:drain.index(";")]
    assert "APP_STOP_TIMEOUT_RECORDING_MS" in call
