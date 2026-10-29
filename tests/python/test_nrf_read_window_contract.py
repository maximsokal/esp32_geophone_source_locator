"""Responses longer than one FIFO transfer are read as 28 byte windows.

The 2026-10-26 logic analyzer capture showed every segmented I2C read of an nRF
target damaged in its tail. These checks keep the wiring from drifting back to
long single reads; the behaviour itself is covered by the host tests
(nrf_read_window, nrf_bootstrap_runtime_zephyr, nrf_topology_log_session and the
topology service integration test, whose fake wire asserts no nRF read is longer
than one window).
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_window_module_is_built_into_both_firmwares():
    esp_component = read("components/nrf_upstream_protocol/CMakeLists.txt")
    nrf_app = read("nRF54L15/zephyr/CMakeLists.txt")
    assert "/src/nrf_read_window.c" in esp_component
    assert "../src/nrf_read_window.c" in nrf_app
    assert (ROOT / "nRF54L15/src/nrf_read_window.c").is_file()
    assert (ROOT / "nRF54L15/include/nrf_read_window.h").is_file()


def test_esp_reads_every_long_nrf_response_through_windows():
    service = read("components/nrf_topology_service/nrf_topology_service.c")

    snapshot = function_body(service, "static esp_err_t platform_read_remote_snapshot(")
    assert "platform_i2c_read_windowed(" in snapshot
    assert "platform_i2c_read_bytes(" not in snapshot

    log_read = function_body(service, "static esp_err_t platform_log_read_once(")
    assert "platform_i2c_read_windowed(" in log_read
    assert "platform_i2c_read_bytes(" not in log_read

    cs_result = function_body(service, "static int cs_poll_result(")
    assert "platform_i2c_read_windowed(" in cs_result
    assert "platform_i2c_read_bytes(" not in cs_result


def test_esp_plain_reads_of_nrf_targets_stay_within_one_window():
    service = read("components/nrf_topology_service/nrf_topology_service.c")
    status = function_body(service, "static esp_err_t platform_read_status(")
    # the status exchange is the 28 byte frame: exactly one window
    assert len(re.findall(r"sizeof\(frame\)", status)) >= 1
    assert "uint8_t frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH]" in status


def test_nrf_runtime_serves_windows_from_the_copy_of_the_last_response():
    runtime = read("nRF54L15/src/nrf_bootstrap_runtime_zephyr.c")
    write = function_body(runtime, "static void target_write_received(")
    reader = function_body(runtime, "static int target_read_requested(")

    assert "nrf_bootstrap_runtime_zephyr_begin_twis_write()" in write
    assert "NRF_UPSTREAM_MESSAGE_READ_WINDOW" in write
    assert "nrf_read_window_target_arm(&s_read_window" in write
    # a window read must come before building a new response, and ordinary
    # reads must record what they handed out
    assert reader.index("nrf_read_window_target_serve(") < reader.index("nrf_cs_control_runtime_take_selected(")
    assert "nrf_read_window_target_record(&s_read_window" in reader
