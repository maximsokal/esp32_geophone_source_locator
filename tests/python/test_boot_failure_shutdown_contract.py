from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = (ROOT / "main/app_storage_boot.c").read_text(encoding="utf-8")
STARTUP = (ROOT / "main/app_startup.c").read_text(encoding="utf-8")
BUTTON = (ROOT / "components/power_control/power_button_adapter.c").read_text(encoding="utf-8")


def test_boot_failure_arms_before_wait_and_cleanup():
    failure = BOOT.split("static void app_storage_boot_failure", 1)[1].split(
        "void app_storage_boot_run_diagnostic", 1
    )[0]
    arm = failure.index("power_control_arm_shutdown_button")
    wait = failure.index("power_control_wait_for_shutdown_request")
    unmount = failure.index("sd_card_unmount")
    shutdown = failure.index("power_control_shutdown")
    assert arm < wait < unmount < shutdown
    assert "if (arm_err == ESP_OK)" in failure
    assert "skipping unreachable wait" in failure


def test_boot_failure_does_not_duplicate_button_state_machine():
    assert "gpio_" not in BOOT
    assert "iot_button" not in BOOT
    assert "BUTTON_LONG_PRESS" not in BOOT


def test_normal_startup_arm_boundary_is_not_moved_early():
    run = STARTUP[STARTUP.index("void app_startup_run(void)") :]
    diagnostic = run.index("app_storage_boot_run_diagnostic();")
    services = run.index("app_startup_initialize_services(")
    arm = run.index("power_control_arm_shutdown_button()")
    assert diagnostic < services < arm
    assert run.count("power_control_arm_shutdown_button()") == 1


def test_held_button_release_confirmation_contract_remains_in_adapter():
    assert "arm_requested" in BUTTON
    assert "BUTTON_PRESS_UP" in BUTTON
    assert "power_button_adapter_arm_after_startup" in BUTTON
    assert "Shutdown arm deferred: button is held" in BUTTON
    assert "atomic_exchange_explicit" in BUTTON
