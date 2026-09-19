from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def numeric_define(text: str, name: str) -> int:
    match = re.search(rf"#define\s+{re.escape(name)}\s+(\d+)U?\b", text)
    assert match, name
    return int(match.group(1))


def test_wire_lengths_fit_explicit_twis_buffer():
    protocol = read("nRF54L15/include/nrf_upstream_protocol.h")
    conf = read("nRF54L15/zephyr/prj.conf")
    frame_len = numeric_define(protocol, "NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH")
    request_len = numeric_define(protocol, "NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH")
    match = re.search(r"CONFIG_I2C_NRFX_TWIS_BUF_SIZE=(\d+)", conf)
    assert match
    buffer_len = int(match.group(1))
    assert request_len == 12
    # 112 bytes as of MAX-114 (altitude_mm + orientation_state/blink_class/
    # tilt_sin2_ppm appended); the previous frame was 102 bytes. Deliberate,
    # reviewed change -- not a silent regression. Still comfortably inside
    # the 128-byte TWIS buffer.
    assert frame_len == 112
    assert frame_len <= buffer_len
    assert buffer_len == 128


def test_remote_frame_keeps_legacy_status_prefix_contract():
    protocol = read("nRF54L15/src/nrf_upstream_protocol.c")
    assert "nrf_upstream_protocol_encode_status(status, frame)" in protocol
    assert "frame[28] = NRF_UPSTREAM_SENSOR_MAGIC_0" in protocol
    # CRC now covers 110 bytes (was 100 before the MAX-114 field extension).
    assert "nrf_upstream_protocol_crc16_ccitt_false(frame, 110U)" in protocol
    assert "nrf_upstream_protocol_decode_status(\n        frame,\n        NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH" in protocol


def test_physical_hil_gate_is_rebased_to_the_full_112_byte_snapshot():
    protocol_doc = read("docs/nrf-upstream-sensor-snapshot-protocol.md")
    matrix = read("docs/verification-evidence-matrix.md")
    architecture = read("ARCHITECTURE.md")
    assert "a complete 112-byte read" in protocol_doc
    assert "112-byte TX buffer" in protocol_doc
    assert "112-byte REMOTE snapshot" in matrix
    assert "112-byte sensor frame" in architecture
    assert "112-byte TWIS response" in architecture
    assert "112-byte polling" in architecture
    assert "full 112-byte TWIS behavior" in architecture
    assert "102-byte sensor frame" not in architecture
    assert "102-byte TWIS response" not in architecture
    assert "102-byte polling" not in architecture
    assert "full 102-byte TWIS behavior" not in architecture

    test = read("tests/host/nrf_upstream_protocol_test.c")
    assert "memcmp(frame, status_frame, sizeof(status_frame)) == 0" in test
    assert "decode_status(frame, NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH" in test


def test_sensor_worker_is_dedicated_and_callback_path_is_nonblocking():
    sensor = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    bootstrap = read("nRF54L15/src/nrf_bootstrap_runtime_zephyr.c")

    assert "K_THREAD_STACK_DEFINE" in sensor
    assert "k_thread_create(" in sensor
    assert "K_MSGQ_DEFINE" in sensor
    assert "K_NO_WAIT" in sensor
    assert "k_work_submit" not in sensor
    assert "nrf_remote_sensor_runtime_zephyr_prepare(" in sensor
    assert "nrf_remote_sensor_runtime_zephyr_refresh(" in sensor

    callback_start = bootstrap.index("static void target_write_received")
    callback_end = bootstrap.index("static int target_read_requested")
    callback = bootstrap[callback_start:callback_end]
    assert "nrf_upstream_protocol_decode_sensor_refresh" in callback
    assert "nrf_sensor_upstream_runtime_zephyr_request_refresh" in callback
    assert "nrf_remote_sensor_runtime" not in callback
    assert "k_msleep" not in callback


def test_remote_logical_activation_precedes_published_assignment_ok():
    sensor = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    bootstrap = read("nRF54L15/src/nrf_bootstrap_runtime_zephyr.c")

    handler_start = bootstrap.index("static void assign_work_handler")
    handler_end = bootstrap.index("static void target_write_received")
    handler = bootstrap[handler_start:handler_end]
    process = handler.index("nrf_bootstrap_process_assign")
    notify = handler.index("nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment")
    publish_ok = handler.rindex("publish_core_status();")
    assert process < notify < publish_ok
    assert re.search(r"result\s*==\s*NRF_UPSTREAM_RESULT_OK", handler)
    assert re.search(r"assign\.role\s*==\s*NRF_UPSTREAM_ROLE_REMOTE", handler)
    assert handler.count("assignment_runtime_failure();") >= 2

    failure_start = bootstrap.index("static void assignment_runtime_failure")
    failure_end = bootstrap.index("static void assign_work_handler")
    failure = bootstrap[failure_start:failure_end]
    assert "publish_core_status_with_result(NRF_UPSTREAM_RESULT_INTERNAL_ERROR);" in failure
    assert "runtime_cold_reboot(NULL);" in failure

    notify_start = sensor.index("int nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment")
    notify_end = sensor.index("int nrf_sensor_upstream_runtime_zephyr_request_refresh")
    notify_body = sensor[notify_start:notify_end]
    activate = notify_body.index("nrf_sensor_session_activate_remote")
    enqueue = notify_body.index("k_msgq_put")
    assert activate < enqueue
    assert "previous_session = s_session;" in notify_body
    assert "s_session = previous_session;" in notify_body

    activation_start = sensor.index("static void handle_activation")
    activation_end = sensor.index("static void sensor_worker")
    activation_body = sensor[activation_start:activation_end]
    assert "nrf_sensor_session_activate_remote" not in activation_body
    assert "active_request_id == 0U" in activation_body


def test_sensor_runtime_owns_private_node_state_not_bootstrap_state_pointer():
    sensor = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    header = read("nRF54L15/include/nrf_sensor_upstream_runtime_zephyr.h")
    assert "static nrf_node_state_t s_worker_node_state;" in sensor
    assert "nrf_node_state_t *" not in header
    assert "nrf_bootstrap" not in sensor
    assert "nrf_bootstrap" not in header


def test_one_firmware_starts_idle_sensor_worker_before_upstream_bootstrap():
    main = read("nRF54L15/src/main.c")
    sensor_start = main.index("nrf_sensor_upstream_runtime_zephyr_start();")
    bootstrap_start = main.index("nrf_bootstrap_runtime_zephyr_start(&node_state);")
    assert sensor_start < bootstrap_start

    cmake = read("nRF54L15/zephyr/CMakeLists.txt")
    assert "../src/nrf_sensor_session.c" in cmake
    assert "../src/nrf_sensor_upstream_runtime_zephyr.c" in cmake
    assert "../src/nrf_bme280_altitude.c" in cmake


def test_altitude_payload_path_is_pure_and_covered_by_host_and_target_builds():
    altitude = read("nRF54L15/src/nrf_bme280_altitude.c")
    runtime = read("nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c")
    cmake = read("nRF54L15/zephyr/CMakeLists.txt")
    manifest = read("tests/host/manifest.d/coverage-nrf.json")

    assert "nrf_bme280_pressure_to_altitude_mm" in altitude
    assert "pow(" not in altitude
    assert "math.h" not in altitude
    assert "nrf_bme280_pressure_to_altitude_mm(snapshot->env.pressure_pa)" in runtime
    assert "../src/nrf_bme280_altitude.c" in cmake
    assert manifest.count("nRF54L15/src/nrf_bme280_altitude.c") == 2


def test_private_i2c_topology_is_preserved_with_max51_bluetooth_enabled():
    overlay = read("nRF54L15/zephyr/app.overlay")
    conf = read("nRF54L15/zephyr/prj.conf")
    assert '&i2c22 {' in overlay
    assert 'compatible = "nordic,nrf-twis";' in overlay
    assert not re.search(r"&i2c30\s*\{[^}]*compatible\s*=\s*\"nordic,nrf-twis\"", overlay, re.S)
    for line in (
        "CONFIG_BT=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_RAS_RREQ=y",
        "CONFIG_BT_RAS_RRSP=y",
    ):
        assert line in conf


def test_host_manifest_covers_new_codec_session_and_zephyr_boundary():
    manifest = read("tests/host/manifest.d/coverage-nrf.json")
    assert '"name": "nrf_upstream_protocol"' in manifest
    assert '"name": "nrf_sensor_session"' in manifest
    assert '"name": "nrf_sensor_upstream_runtime_zephyr"' in manifest
    assert "tests/host/stubs/nrf_sensor_upstream_runtime_zephyr" in manifest
