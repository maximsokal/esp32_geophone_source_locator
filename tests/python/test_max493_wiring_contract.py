from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_watchdog_activation_only_starts_a_new_grace_period_on_transition():
    source = (ROOT / "nRF54L15/src/nrf_watchdog_health.c").read_text(encoding="utf-8")
    assert "if (active && !health->active[participant])" in source


def test_sensor_worker_bounds_wait_to_active_stage_signal_phase():
    source = (ROOT / "nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c").read_text(
        encoding="utf-8"
    )
    assert "s_stage_signal.phase_remaining_ms" in source
    assert "if (stage_remaining < wait_ms)" in source
    assert "return K_MSEC(wait_ms);" in source


def test_sensor_worker_priority_remains_preemptive_ten():
    source = (ROOT / "nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c").read_text(
        encoding="utf-8"
    )
    assert "#define NRF_SENSOR_WORKER_PRIORITY K_PRIO_PREEMPT(10)" in source


def test_main_tick_uses_phase_locked_deadline():
    source = (ROOT / "nRF54L15/src/main.c").read_text(encoding="utf-8")
    assert "next_main_tick_ms = nrf_timing_next_periodic_deadline_ms(" in source
    assert "next_main_tick_ms = now_ms + MAIN_TICK_PERIOD_MS;" not in source
