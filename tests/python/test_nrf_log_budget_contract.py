from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_log_budgets_fit_a_complete_owned_session():
    config = (ROOT / "components/nrf_topology_service/Kconfig").read_text()
    node, rest = config.split("config NRF_LOG_POLL_NODE_BUDGET_MS", 1)[1].split(
        "config NRF_LOG_POLL_ITERATION_BUDGET_MS", 1
    )
    iteration = rest.split("config NRF_LOG_POLL_PERIOD_MS", 1)[0]
    assert "range 120 1000" in node and "default 120" in node
    assert "range 120 2000" in iteration and "default 120" in iteration
    service = (ROOT / "components/nrf_topology_service/nrf_topology_service.c").read_text()
    assert "_Static_assert(CONFIG_NRF_LOG_POLL_NODE_BUDGET_MS >" in service
    assert "_Static_assert(CONFIG_NRF_LOG_POLL_ITERATION_BUDGET_MS >" in service
    assert ".set_deadline = platform_log_set_deadline" in service
    assert ".scheduler_reserve_ms = 1000U / configTICK_RATE_HZ" in service


def test_native_driver_reuses_remaining_budget_for_all_command_waits():
    source = (ROOT / "components/esp_driver_i2c/i2c_master.c").read_text()
    body = source.split("static void s_i2c_send_commands(", 1)[1].split(
        "static void s_i2c_send_command_async", 1
    )[0]
    assert "const TickType_t started_tick = xTaskGetTickCount();" in body
    assert "xSemaphoreTake(i2c_master->cmd_semphr, remaining)" in body
    assert body.count("i2c_timeout_remaining_ticks(") == 3
    assert "xSemaphoreTake(i2c_master->cmd_semphr, ticks_to_wait)" not in body
    assert "&event, ticks_to_wait" not in body
