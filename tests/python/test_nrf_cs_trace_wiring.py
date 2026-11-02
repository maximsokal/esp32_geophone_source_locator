"""The console trace of Channel Sounding must stay wired in and must stay off the I2C-readable log ring.

Bench log 2026-11-02: the ESP32 log showed how every attempt ended ("timeout in SECURING") but the nRF console had
no Channel Sounding line at all, so the time of the stages (connect, security, ...) a per-pair budget is built from
could not be read. The trace prints from the main loop; the BLE work items only push a record.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_the_trace_is_built_into_the_production_firmware_and_the_native_test_firmware():
    for cmake in ("nRF54L15/zephyr/CMakeLists.txt", "nRF54L15/tests/ncs_native/CMakeLists.txt"):
        text = _read(cmake)
        assert "nrf_cs_trace.c" in text and "nrf_cs_trace_zephyr.c" in text, cmake


def test_the_main_loop_prints_the_trace_and_the_ble_handlers_only_push_to_it():
    main = _read("nRF54L15/src/main.c")
    assert '#include "nrf_cs_trace_zephyr.h"' in main
    loop = main[main.index("while (1) {"):]
    assert "nrf_cs_trace_zephyr_console_drain();" in loop

    zephyr = _read("nRF54L15/src/nrf_cs_trace_zephyr.c")
    drain = zephyr.index("void nrf_cs_trace_zephyr_console_drain(void)\n{")
    # printk is only in the drain, which the main loop runs: never in a BLE callback or work item.
    assert "printk" not in zephyr[:drain].replace("#include <zephyr/sys/printk.h>", "")

    runtime = "".join(_read(f"nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_{i}.inc") for i in (1, 2, 3))
    for hook in ("nrf_cs_trace_zephyr_execute(", "nrf_cs_trace_zephyr_event(", "nrf_cs_trace_zephyr_timeout(",
                 "nrf_cs_trace_zephyr_result(", "nrf_cs_trace_zephyr_rf("):
        assert hook in runtime, hook
    assert "printk" not in runtime


def test_the_trace_does_not_use_the_log_ring_the_esp32_reads_over_i2c():
    for path in ("nRF54L15/src/nrf_cs_trace.c", "nRF54L15/src/nrf_cs_trace_zephyr.c",
                 "nRF54L15/include/nrf_cs_trace.h", "nRF54L15/include/nrf_cs_trace_zephyr.h"):
        text = _read(path)
        assert "nrf_log_runtime_zephyr" not in text and "nrf_log_ring" not in text, path
