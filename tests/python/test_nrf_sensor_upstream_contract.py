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
    assert frame_len == 102
    assert frame_len <= buffer_len
    assert buffer_len == 128


def test_remote_frame_keeps_legacy_status_prefix_contract():
    protocol = read("nRF54L15/src/nrf_upstream_protocol.c")
    assert "nrf_upstream_protocol_encode_status(status, frame)" in protocol
    assert "frame[28] = NRF_UPSTREAM_SENSOR_MAGIC_0" in protocol
    assert "nrf_upstream_protocol_crc16_ccitt_false(frame, 100U)" in protocol
    assert "nrf_upstream_protocol_decode_status(\n        frame,\n        NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH" in protocol

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
    assert "result == NRF_UPSTREAM_RESULT_OK" in handler
    assert "assign.role == NRF_UPSTREAM_ROLE_REMOTE" in handler
    assert "publish_core_status_with_result(NRF_UPSTREAM_RESULT_INTERNAL_ERROR)" in handler
    assert "runtime_cold_reboot(NULL)" in handler

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


def test_private_i2c_topology_and_no_bluetooth_scope_creep():
    overlay = read("nRF54L15/zephyr/app.overlay")
    conf = read("nRF54L15/zephyr/prj.conf")
    assert '&i2c22 {' in overlay
    assert 'compatible = "nordic,nrf-twis";' in overlay
    assert not re.search(r"&i2c30\s*\{[^}]*compatible\s*=\s*\"nordic,nrf-twis\"", overlay, re.S)
    assert "CONFIG_BT=y" not in conf
    assert "CONFIG_BT_CTLR" not in conf


def test_host_manifest_covers_new_codec_session_and_zephyr_boundary():
    manifest = read("tests/host/manifest.d/coverage-nrf.json")
    assert '"name": "nrf_upstream_protocol"' in manifest
    assert '"name": "nrf_sensor_session"' in manifest
    assert '"name": "nrf_sensor_upstream_runtime_zephyr"' in manifest
    assert "tests/host/stubs/nrf_sensor_upstream_runtime_zephyr" in manifest
