from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_target_harness_builds_the_real_service_and_calls_public_leveling_api():
    cmake = read("tests/target/test_app/CMakeLists.txt")
    main_cmake = read("tests/target/test_app/main/CMakeLists.txt")
    harness = read("tests/target/test_app/main/test_app_nrf_topology.c")
    header = read("components/nrf_topology_service/include/nrf_topology_service.h")
    service = read("components/nrf_topology_service/nrf_topology_service.c")

    assert "EXTRA_COMPONENT_DIRS" in cmake
    assert "NRF_TOPOLOGY_SERVICE_TEST_HARNESS=1" in cmake
    assert "REQUIRES nrf_topology_service" in main_cmake
    assert "nrf_topology_service_get_leveling_summary(&summary)" in harness
    assert "nrf_topology_service_test_publish_state" in harness
    assert "nrf_topology_service_test_publish_state" in header
    assert "nrf_topology_leveling_summary_compute" in service
