from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_topology_snapshot_carries_assignment_epoch():
    header = read("components/nrf_topology_service/include/nrf_topology.h")
    core = read("components/nrf_topology_service/nrf_topology.c")
    assert "uint32_t assignment_request_id;" in header
    assert "node->assignment_request_id = status->request_id;" in core


def test_sensor_transport_uses_canonical_max36_codec_at_platform_boundary():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert "NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH" in service
    assert "nrf_upstream_protocol_encode_sensor_refresh" in service
    assert "NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH" in service
    assert "nrf_upstream_protocol_decode_remote_snapshot" in service
    assert "NRF_UPSTREAM_ADDRESS_DISCOVERY" not in read(
        "components/nrf_topology_service/nrf_sensor_transport.c"
    )


def test_one_esp32_tca_owner_and_no_second_sensor_task():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    public = read("components/nrf_topology_service/include/nrf_topology_service.h")
    assert service.count("xTaskCreatePinnedToCore(") == 1
    assert '"nrf_topology"' in service
    assert "nrf_topology_service_request_sensor_refresh" in public
    assert "nrf_topology_service_request_all_sensor_refresh" in public
    assert "nrf_topology_service_get_sensor_snapshot" in public
    assert "nrf_sensor_transport.h" not in public
    assert "i2c_bus" not in public
    assert "tca9548a" not in public


def test_pending_mask_is_explicit_and_duplicate_requests_coalesce():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert "static uint8_t s_sensor_pending_mask" in service
    assert "static uint8_t s_sensor_inflight_mask" in service
    assert "s_sensor_pending_mask | s_sensor_inflight_mask" in service
    assert "service_take_sensor_mask" in service
    assert "service_requeue_inflight_mask" in service


def test_sensor_wakeups_do_not_reset_periodic_topology_deadline():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert "tick_period_due" in service
    assert "tick_wait_until_period" in service
    assert "last_reconcile_tick" in service
    assert "service_has_immediate_work" in service
    assert "ulTaskNotifyTake(pdTRUE, wait_ticks)" in service
    assert "ulTaskNotifyTake(\n            pdTRUE,\n            pdMS_TO_TICKS(NRF_TOPOLOGY_RECONCILE_PERIOD_MS))" not in service


def test_initial_refresh_is_identity_scoped_not_periodic_sensor_polling():
    transport = read("components/nrf_topology_service/nrf_sensor_transport.c")
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert "initial_refresh_issued" in transport
    assert "last_attempt_state" in transport
    assert "nrf_sensor_cache_reconcile_topology" in service
    assert "service_queue_sensor_mask(initial_mask, false);" in service
    assert service.count("service_queue_sensor_mask(initial_mask, false);") == 1


def test_component_keeps_existing_task_policy_and_adds_timer_only():
    cmake = read("components/nrf_topology_service/CMakeLists.txt")
    task_config = read("components/app_config/include/app_task_config.h")
    assert '"nrf_sensor_transport.c"' in cmake
    assert re.search(r"\besp_timer\b", cmake)
    assert re.search(r"#define\s+APP_TASK_PRIORITY_NRF_TOPOLOGY\s+1\b", task_config)
    assert re.search(r"#define\s+APP_TASK_STACK_NRF_TOPOLOGY\s+4096\b", task_config)


def test_i2c_bus_pin_and_shutdown_owner_contracts_remain_intact():
    lock = read("dependencies.lock")
    shutdown = read("main/app_shutdown.c")
    assert "espressif/i2c_bus:" in lock
    assert "version: 1.5.2" in lock
    topology = shutdown.index("nrf_topology_service_stop();")
    bus = shutdown.index("sensor_bus_deinit();")
    assert topology < bus
