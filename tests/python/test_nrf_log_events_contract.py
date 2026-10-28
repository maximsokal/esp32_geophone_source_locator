"""MAX-308 contract for the shared nRF log event definition."""

import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
DEF = ROOT / "nRF54L15" / "include" / "nrf_log_events.def"
ROW = re.compile(r"X\(\s*(\d+)\s*,\s*([A-Z][A-Z0-9_]*)\s*,\s*([A-Z][A-Z0-9_]*)\s*,\s*(DEBUG|INFO|WARN|ERROR|FATAL)\s*\)")


def events():
    parsed = []
    for line in DEF.read_text(encoding="utf-8").splitlines():
        match = ROW.fullmatch(line.strip())
        assert match, f"invalid nrf_log_events.def row: {line!r}"
        parsed.append((int(match.group(1)), match.group(2), match.group(3), match.group(4)))
    assert len({row[0] for row in parsed}) == len(parsed), "duplicate event id"
    assert len({row[1] for row in parsed}) == len(parsed), "duplicate event name"
    return parsed


def test_event_definition_and_generated_typescript_are_current():
    assert subprocess.run(
        [sys.executable, "tools/gen_nrf_log_events.py", "--check"], cwd=ROOT, check=False
    ).returncode == 0
    expected_names = {
        "BOOT", "LOG_GENERATION_ROLLOVER", "TWIS_INVALID_FRAME", "TWIS_UNKNOWN_TYPE", "TWIS_BUSY",
        "ROLE_ASSIGNED", "ROLE_LOST", "IMU_FAULT_SET", "IMU_FAULT_CLEARED", "ENV_FAULT_SET",
        "ENV_FAULT_CLEARED", "HEIGHT_REF_REJECTED", "IMU_CAL_REJECTED", "CS_EXECUTE_FAILED",
        "CS_TIMEOUT", "WATCHDOG_PARTICIPANT_STALE", "LOG_TEST_EVENT", "FAULT_RECORD_WORD",
        "BOOT_FAILED", "CS_EVENT_QUEUE_OVERFLOW", "WATCHDOG_DEGRADED", "WATCHDOG_FEED_FAILED", "SENSOR_INIT_FAILED",
        "LOGGER_LATCH", "SENSOR_SCAN_WORD0", "SENSOR_SCAN_WORD1", "SENSOR_SCAN_WORD2",
        "SENSOR_SCAN_WORD3", "SENSOR_SCAN_DONE", "SENSOR_BUS_FAULT",
    }
    parsed = events()
    assert {row[1] for row in parsed} == expected_names
    assert [row[0] for row in parsed] == list(range(1, len(parsed) + 1))


def test_every_event_has_english_and_ukrainian_web_copy():
    catalog = json.loads((ROOT / "i18n" / "web.json").read_text(encoding="utf-8"))
    entries = catalog["entries"]
    for _, name, _, _ in events():
        entry = entries[f"nrf_log.event.{name.lower()}"]
        assert entry["en"].strip()
        assert entry["uk"].strip()
        assert "\ufffd" not in entry["en"]
        assert "\ufffd" not in entry["uk"]


def test_both_firmware_builds_compile_the_same_event_table_source():
    shared_source = (ROOT / "nRF54L15" / "src" / "nrf_log_protocol.c").read_text(encoding="utf-8")
    assert '#include "nrf_log_events.def"' in shared_source
    esp_component = (ROOT / "components" / "nrf_upstream_protocol" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    nrf_component = (ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "${NRF_UPSTREAM_PROTOCOL_ROOT}/src/nrf_log_protocol.c" in esp_component
    assert "../src/nrf_log_protocol.c" in nrf_component
