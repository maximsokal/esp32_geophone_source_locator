from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def test_i2c_probe_diagnostics_kconfig_defaults_are_bounded():
    kconfig = _read("main/Kconfig.projbuild")

    assert "config APP_I2C_PROBE_DIAGNOSTICS\n" in kconfig
    assert "config APP_I2C_PROBE_DIAGNOSTICS_FULL_PASSES\n" in kconfig
    diagnostics = kconfig.index("config APP_I2C_PROBE_DIAGNOSTICS\n")
    full_passes = kconfig.index("config APP_I2C_PROBE_DIAGNOSTICS_FULL_PASSES\n")
    end_menu = kconfig.index("endmenu", full_passes)
    assert "default y" in kconfig[diagnostics:full_passes]
    assert "range 0 64" in kconfig[full_passes:end_menu]
    assert "default 3" in kconfig[full_passes:end_menu]


def test_every_nrf_read_and_write_uses_the_diagnostic_transaction_wrapper():
    service = _read("components/nrf_topology_service/nrf_topology_service.c")

    assert service.count("sensor_bus_read_bytes(") == 1
    assert service.count("sensor_bus_write_bytes(") == 1
    assert "i2c_bus_read_bytes(" not in service
    assert "i2c_bus_write_bytes(" not in service
    assert "SENSOR_BUS_PROBE_PHASE_READ" in service
    assert "SENSOR_BUS_PROBE_PHASE_WRITE" in service
    assert "sensor_bus_probe_diag_begin_pass(platform->reconcile_pass_count)" in service
    assert "sensor_bus_probe_diag_end_pass();" in service
    assert "sensor_bus_probe_diag_log_bus_idle();" in service


def test_tca_control_callbacks_keep_phase_channel_and_expected_readback():
    mux = _read("components/tca9548a_mux/tca9548a_mux.c")
    adapter = _read("components/tca9548a_mux/tca9548a_mux_i2c_bus.c")

    assert "TCA9548A_MUX_PHASE_CLOSE" in mux
    assert "TCA9548A_MUX_PHASE_SELECT" in mux
    assert "TCA9548A_MUX_PHASE_VERIFY" in mux
    assert "sensor_bus_probe_diag_transaction(" in adapter
    assert "uint8_t expected," in adapter
    assert "tca9548a_diag_phase(phase), address, channel" in adapter


def test_transaction_log_captures_timing_and_line_levels_around_driver_call():
    diag = _read("components/sensor_bus/sensor_bus_probe_diag.c")
    sensor_bus = _read("components/sensor_bus/sensor_bus.c")

    for field in (
        "phase=%s",
        "addr=0x%02X",
        "ch=direct",
        "tx=%s rx=%s exp=%s",
        "err=0x%X(%s)",
        "us=%llu",
        "lvl=SDA%d/SCL%d>%d%d",
    ):
        assert field in diag
    assert "s_storage.started_us = esp_timer_get_time();\n    err = transfer(context, tx, tx_len, rx, rx_len);\n    s_storage.finished_us = esp_timer_get_time();" in diag
    assert "sensor_bus_lines_read(&s_storage.sda_before, &s_storage.scl_before);" in diag
    assert "sensor_bus_lines_read(&s_storage.sda_after, &s_storage.scl_after);" in diag
    assert "s_storage.transaction.err = err;" in diag
    assert "config.sda_io_num" in sensor_bus
    assert "config.scl_io_num" in sensor_bus
    assert "config.master.clk_speed" in sensor_bus
    assert "config.sda_pullup_en" in sensor_bus
    assert "config.scl_pullup_en" in sensor_bus
    assert "SENSOR_BUS_MUTEX_WAIT_MS" in sensor_bus
    assert "SENSOR_BUS_TRANSACTION_TIMEOUT_MS" in sensor_bus
    assert "SENSOR_BUS_I2C_GLITCH_IGNORE_COUNT" in sensor_bus
    assert "probe_timeout_ms=%u" in sensor_bus


def test_probe_helper_has_fixed_bounds_and_no_heap_or_waits():
    policy = _read("components/sensor_bus/include/sensor_bus_probe_diag_policy.h")
    policy_source = _read("components/sensor_bus/sensor_bus_probe_diag_policy.c")
    diag = _read("components/sensor_bus/sensor_bus_probe_diag.c")

    assert "SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT 32U" in policy
    assert "SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES 6144U" in policy
    assert "SENSOR_BUS_PROBE_DIAG_EVENT_LIMIT" not in policy
    assert "events[" not in policy
    assert "event_count" not in policy_source
    assert "_Static_assert(sizeof(sensor_bus_probe_diag_storage_t) <=" in diag
    assert "static sensor_bus_probe_diag_storage_t s_storage;" in diag
    assert "SENSOR_BUS_PROBE_DIAG_SUMMARY_INTERVAL_US UINT64_C(60000000)" in policy
    assert "malloc(" not in diag
    assert "vTaskDelay(" not in diag
    assert "ESP_LOGI(TAG, \"I2C_BUS idle lvl=SDA%d/SCL%d\"" in diag


def test_transaction_prints_are_bounded_and_deferred_during_mux_ownership():
    policy_source = _read("components/sensor_bus/sensor_bus_probe_diag_policy.c")
    diag = _read("components/sensor_bus/sensor_bus_probe_diag.c")

    assert "bool *should_print" in policy_source
    assert "emit_transaction(&s_storage.transaction);" in diag
    assert "s_storage.pending_count < SENSOR_BUS_PROBE_DIAG_PENDING_LIMIT" in diag
    assert "if (!s_storage.output_deferred)" in diag
    assert "I2C_PROBE_SUMMARY transactions_dropped=%lu" in diag
    service = _read("components/nrf_topology_service/nrf_topology_service.c")
    # Mux close, mux select and the direct MAIN log node each take bus ownership,
    # defer probe output while holding it and flush it on release.
    assert service.count("sensor_bus_probe_diag_defer_output();") == 3
    assert service.count("sensor_bus_owner_release();\n    sensor_bus_probe_diag_flush();") == 1
    # mux select failure, MAIN log begin failure and MAIN log end.
    assert service.count("sensor_bus_owner_release();\n        sensor_bus_probe_diag_flush();") == 3
    log_begin = service.split("static esp_err_t platform_log_begin_node", 1)[1]
    log_begin, log_end = log_begin.split("static esp_err_t platform_log_end_node", 1)
    assert "sensor_bus_probe_diag_defer_output();" in log_begin
    assert "sensor_bus_probe_diag_flush();" in log_begin
    assert "sensor_bus_probe_diag_flush();" in log_end.split("static ", 1)[0]
    assert "I2C_PROBE_SUMMARY groups_dropped=%lu" in diag
