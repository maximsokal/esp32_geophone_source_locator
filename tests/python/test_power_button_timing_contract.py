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


def test_shutdown_cannot_arm_until_startup_explicitly_requests_it():
    adapter = read("components/power_control/power_button_adapter.c")
    startup = read("main/app_startup.c")
    public_api = read("components/power_control/include/power_control.h")

    assert "power_control_arm_shutdown_button" in public_api
    assert "power_button_adapter_arm_after_startup" in adapter
    assert "atomic_bool arm_requested" in adapter
    assert "atomic_bool armed" in adapter
    assert "POWER_BUTTON_RELEASE_CONFIRM_MS 50U" in adapter
    assert "Shutdown arm deferred: button is held at startup completion" in adapter
    assert "Shutdown button armed after debounced release" in adapter

    # The physical input object exists early, but the shutdown latch is opened
    # only after runtime services/UI/acquisition startup has returned.
    services_call = startup.index("app_startup_initialize_services(", startup.index("void app_startup_run"))
    arm_call = startup.index("power_control_arm_shutdown_button()", services_call)
    success = startup.index("app_startup_log_success();", arm_call)
    assert services_call < arm_call < success


def test_boot_release_event_does_not_implicitly_arm_shutdown():
    adapter = read("components/power_control/power_button_adapter.c")

    press_up_block = adapter[
        adapter.index("if (event == BUTTON_PRESS_UP)") :
        adapter.index("if (\n        event != BUTTON_LONG_PRESS_START")
    ]
    assert "arm_requested" in press_up_block
    assert "atomic_exchange_explicit" in press_up_block
    assert "state->armed" in press_up_block


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

    assert "real raw GPIO21-edge to latch measurement" in plan
    assert "software debounce marker is not that physical measurement" in plan
    assert "configured `long_press_time` of\n150 ms" in architecture
    assert "not a raw\nGPIO21-to-latch timing claim" in architecture
    assert "explicit HIL measurement" in architecture
