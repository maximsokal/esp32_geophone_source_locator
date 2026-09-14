from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_shared_codec_has_one_canonical_source():
    cmake = read("components/nrf_upstream_protocol/CMakeLists.txt")
    assert "../../nRF54L15" in cmake
    assert "/src/nrf_upstream_protocol.c" in cmake
    assert "/include" in cmake

    duplicates = list((ROOT / "components").rglob("nrf_upstream_protocol.c"))
    assert duplicates == []
    assert (ROOT / "nRF54L15/src/nrf_upstream_protocol.c").is_file()


def test_topology_service_starts_after_interactive_startup_and_before_shutdown_wait():
    main = read("main/main.c")
    startup = main.index("app_startup_run();")
    topology = main.index("nrf_topology_service_start();")
    shutdown = main.index("app_shutdown_wait_and_run();")
    assert startup < topology < shutdown
    assert "nRF topology service unavailable" in main


def test_shutdown_stops_topology_before_shared_i2c_owner():
    shutdown = read("main/app_shutdown.c")
    topology = shutdown.index("nrf_topology_service_stop();")
    ina = shutdown.index("ina3221_monitor_stop();")
    bus = shutdown.index("sensor_bus_deinit();")
    assert topology < ina < bus


def test_task_policy_and_service_defaults_are_centralized():
    task_config = read("components/app_config/include/app_task_config.h")
    assert re.search(r"#define\s+APP_TASK_PRIORITY_NRF_TOPOLOGY\s+1\b", task_config)
    assert re.search(r"#define\s+APP_TASK_STACK_NRF_TOPOLOGY\s+4096\b", task_config)

    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert "APP_TASK_CORE_SERVICES" in service
    assert "APP_TASK_PRIORITY_NRF_TOPOLOGY" in service
    assert "APP_TASK_STACK_NRF_TOPOLOGY" in service
    assert "NRF_TOPOLOGY_RECONCILE_PERIOD_MS 5000U" in service

    header = read("components/nrf_topology_service/include/nrf_topology.h")
    assert "NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS 500U" in header
    assert "NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS 10U" in header
    assert "NRF_TOPOLOGY_REMOTE_COUNT 8U" in header


def test_service_owns_only_nrf_address_handles_not_shared_bus_or_tca():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    assert service.count("i2c_bus_device_create(") == 3
    assert "platform_delete_device(&platform->remote_device);" in service
    assert "platform_delete_device(&platform->main_device);" in service
    assert "platform_delete_device(&platform->discovery_device);" in service
    assert "platform->tca_device = NULL;" in service
    assert "i2c_bus_delete(" not in service
    assert "sensor_bus_get_tca9548a" in service


def test_component_dependencies_keep_sensor_bus_as_i2c_owner():
    topology_cmake = read("components/nrf_topology_service/CMakeLists.txt")
    for dependency in (
        "nrf_upstream_protocol",
        "sensor_bus",
        "tca9548a_mux",
        "i2c_bus",
        "app_config",
    ):
        assert dependency in topology_cmake

    main_cmake = read("main/CMakeLists.txt")
    assert "nrf_topology_service" in main_cmake

    sensor_bus = read("components/sensor_bus/sensor_bus.c")
    assert sensor_bus.count("i2c_bus_create(") == 1
