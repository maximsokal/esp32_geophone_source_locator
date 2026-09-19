from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_orientation_is_pure_integer_and_explicit_axis():
    header = read("nRF54L15/include/nrf_orientation.h")
    source = read("nRF54L15/src/nrf_orientation.c")
    assert "NRF_ORIENTATION_AXIS_UNCONFIGURED" in header
    for token in ("POS_X", "NEG_X", "POS_Y", "NEG_Y", "POS_Z", "NEG_Z"):
        assert f"NRF_ORIENTATION_AXIS_{token}" in header
    assert "<math.h>" not in source
    assert "acos(" not in source
    assert "atan" not in source
    assert "sin(" not in source
    assert "cos(" not in source
    assert "NRF_ORIENTATION_DEFAULT_ACCEPT_SIN2_PPM" in header
    assert "NRF_ORIENTATION_DEFAULT_REENTER_SIN2_PPM" in header


def test_worker_remains_single_thread_and_uses_timed_msgq_wait():
    source = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    assert source.count("k_thread_create(") == 1
    assert "k_msgq_get(\n            &s_sensor_queue,\n            &event,\n            orientation_wait_timeout())" in source
    assert "K_MSEC(remaining)" in source
    assert "k_timer" not in source
    assert "k_work" not in source


def test_periodic_orientation_is_imu_only_and_aggregate_refresh_is_reused():
    source = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    periodic = source[source.index("static void orientation_periodic_read"):source.index("static void orientation_consume_aggregate_refresh")]
    assert "sources.read_imu" in periodic
    assert "read_env" not in periodic
    assert "nrf_bme280" not in periodic
    assert "orientation_consume_aggregate_refresh(did_refresh);" in source
    assert "NRF_SENSOR_VALID_IMU" in source


def test_role_and_axis_gates_prevent_unapproved_periodic_io():
    source = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    enabled = source[source.index("static bool orientation_periodic_enabled"):source.index("static k_timeout_t orientation_wait_timeout")]
    assert "NRF_ORIENTATION_AXIS_UNCONFIGURED" in enabled
    assert "NRF_NODE_ROLE_REMOTE" in enabled
    assert "s_runtime.core.imu_ready" in enabled
    assert "s_runtime.core.sources.read_imu != NULL" in enabled


def test_orientation_configuration_is_explicit_and_prestart_only():
    header = read("nRF54L15/include/nrf_sensor_upstream_runtime_zephyr.h")
    source = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    assert "nrf_sensor_upstream_runtime_zephyr_configure_orientation" in header
    assert "nrf_sensor_upstream_runtime_zephyr_get_orientation" in header
    assert "if (s_started)" in source
    assert "return -EBUSY;" in source
    assert "nrf_orientation_default_config(&s_orientation_config);" in source


def test_orientation_deadline_skips_catchup_bursts():
    source = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    advance = source[source.index("static void orientation_advance_deadline"):source.index("static void orientation_reset")]
    assert "target = sample_uptime_ms + period" in advance
    assert "steps = (delta + period - 1U) / period" in advance
    assert "while (" not in advance


def test_wire_protocol_is_unchanged_and_max51_ble_surface_is_enabled():
    protocol = read("nRF54L15/include/nrf_upstream_protocol.h")
    prj = read("nRF54L15/zephyr/prj.conf")
    assert "#define NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH 12U" in protocol
    # 112U as of MAX-114, which is precisely the orientation-state upstream
    # extension this test file predates -- the frame length changing here
    # is the point of MAX-114, not a regression.
    assert "#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH 112U" in protocol
    # This test used to assert "orientation" is absent from the wire
    # protocol -- true when this test was written (orientation stayed
    # local to the nRF), and precisely the gap MAX-114 closed on purpose:
    # orientation_state/orientation_blink_class/orientation_tilt_sin2_ppm
    # are now part of the snapshot frame so ESP32 can gate on leveling.
    assert "orientation_state" in protocol
    for line in (
        "CONFIG_BT=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_RAS_RREQ=y",
        "CONFIG_BT_RAS_RRSP=y",
    ):
        assert line in prj


def test_build_and_host_manifests_include_orientation_without_new_firmware_variant():
    cmake = read("nRF54L15/zephyr/CMakeLists.txt")
    manifest = read("tests/host/manifest.d/coverage-nrf.json")
    platformio = read("nRF54L15/platformio.ini")
    assert "../src/nrf_orientation.c" in cmake
    assert '"name": "nrf_orientation"' in manifest
    assert '"nRF54L15/src/nrf_orientation.c"' in manifest
    envs = re.findall(r"^\[env:([^]]+)\]", platformio, flags=re.MULTILINE)
    assert envs == ["seeed-xiao-nrf54l15", "native"]
