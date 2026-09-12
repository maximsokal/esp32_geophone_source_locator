"""Static and replay-facing contracts for physical power-button timing."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_power_button_reports_debounced_timing_after_authoritative_latch():
    source = read("components/power_control/power_button_adapter.c")

    assert "BUTTON_PRESS_DOWN" in source
    assert "esp_log_timestamp" in source
    assert "POWER_BUTTON_TIMING:" in source
    assert "debounced_down_ms" in source
    assert "latch_ms" in source
    assert "debounced_elapsed_ms" in source
    assert source.index("xEventGroupSetBits(state->events, POWER_EVENT_SHUTDOWN_REQUESTED)") < source.index("POWER_BUTTON_TIMING:")


def test_pipeline_requires_explicit_raw_gpio_timing_attestation():
    runner = read("tools/run_pipeline_hil.ps1")

    for token in (
        "ConfirmPowerButtonTiming",
        "PowerButtonTimingMs",
        "PowerButtonTimingTargetMs",
        "PowerButtonTimingToleranceMs",
        "GPIO21 HIGH",
        "Power button timing marker",
    ):
        assert token in runner
    assert '"Power button timing marker"' in runner
    assert 'if ($ConfirmPowerButtonTiming)' in runner


def test_docs_keep_raw_and_debounced_power_timing_separate():
    plan = read("docs/firmware-test-plan.md")
    architecture = read("ARCHITECTURE.md")

    assert "GPIO21 HIGH" in plan
    assert "shutdown latch" in plan
    assert "debounced_elapsed_ms" in plan
    assert "Do not infer" in plan
    assert "configured 150 ms Button long-press value" in plan
    assert "not a raw" in architecture
