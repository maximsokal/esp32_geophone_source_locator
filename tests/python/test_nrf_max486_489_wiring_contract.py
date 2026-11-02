import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def manifest_entry(name: str) -> tuple[Path, dict]:
    for path in (ROOT / "tests" / "host" / "manifest.d").glob("*.json"):
        document = json.loads(path.read_text(encoding="utf-8"))
        for item in document.get("tests", []):
            if item.get("name") == name:
                return path, item
    raise AssertionError(f"host test {name} is not registered in a fragment")


def test_new_host_tests_are_registered_and_reachable_from_main():
    cases = (
        ("nrf_fault_record", "tests/host/nrf_fault_record_test.c", "test_fault_record_round_trip_and_checksum"),
        ("nrf_thread_stack_inventory", "tests/host/nrf_thread_stack_inventory_test.c", "test_stack_inventory_is_named_and_sizes_are_bytes"),
        ("nrf_boot_failure_reason", "tests/host/nrf_boot_failure_reason_test.c", "test_resource_exhaustion_maps_to_a_stable_reason"),
        ("nrf_cs_event_overflow", "tests/host/nrf_cs_event_overflow_test.c", "test_event_overflow_counter_saturates"),
        ("nrf_boot_diagnostics", "tests/host/nrf_boot_diagnostics_test.c", "test_twis_spsc_commit_interleave"),
        ("nrf_watchdog_health", "tests/host/nrf_watchdog_health_test.c", "test_active_bt_cs_progress_participant_stops_and_resumes_feed"),
    )
    for name, source_path, test_name in cases:
        _, entry = manifest_entry(name)
        sources = {Path(source).as_posix() for source in entry.get("sources", [])}
        assert source_path in sources
        source = read(source_path)
        assert re.search(rf"static void {test_name}\s*\(", source)
        assert re.search(rf"\b{test_name}\s*\(\s*\);", source[source.index("int main("):])
        if name == "nrf_boot_diagnostics":
            assert "NRF_BOOT_DIAGNOSTICS_TEST_HOOKS" in entry.get("defines", [])


def test_wdt_configuration_and_progress_contract_are_wired():
    config = read("nRF54L15/zephyr/prj.conf")
    assert "CONFIG_WATCHDOG=y" in {line.strip() for line in config.splitlines()}
    runtime = read("nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_3.inc")
    connections = read("nRF54L15/src/nrf_cs_pair_runtime_zephyr_connections.inc")
    assert '#include "nrf_cs_pair_runtime_zephyr_connections.inc"' in runtime
    disconnect = connections[connections.index("static void disconnected_cb"):]
    assert "nrf_watchdog_zephyr_checkin" not in disconnect
    assert "nrf_watchdog_zephyr_set_participant_active" not in disconnect
    assert "NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS" in runtime
    event_work = runtime[runtime.index("static void event_work_handler"):runtime.index('#include "nrf_cs_pair_runtime_zephyr_connections.inc"')]
    assert "event.type != RUNTIME_EVENT_DISCONNECTED" in event_work
    assert "event.type != RUNTIME_EVENT_INCOMING_DISCONNECTED" in event_work
    _, watchdog = manifest_entry("nrf_watchdog_runtime_zephyr")
    assert "tests/host/nrf_watchdog_runtime_zephyr_test.c" in watchdog.get("sources", [])


def test_fatal_and_boot_failure_events_keep_status_v1_and_reach_the_collector():
    events = read("nRF54L15/include/nrf_log_events.def")
    for name in ("FAULT_RECORD_WORD", "BOOT_FAILED", "CS_EVENT_QUEUE_OVERFLOW", "WATCHDOG_DEGRADED", "WATCHDOG_FEED_FAILED"):
        assert name in events
    main = read("nRF54L15/src/main.c")
    assert "NRF_LOG_EVENT_BOOT_FAILED" in main
    assert main.index("nrf_cs_pair_runtime_zephyr_start()") < main.index("NRF_LOG_EVENT_BOOT_FAILED")
    assert main.index("NRF_LOG_EVENT_BOOT_FAILED") < main.index("nrf_bootstrap_runtime_zephyr_start(&node_state)")
    collector_test = read("tests/host/nrf_log_collector_test.c")
    assert "test_fault_record_words_are_collected_without_decoding" in collector_test
    assert "page.records[index].arg1 == field_value" in collector_test
    _, collector = manifest_entry("nrf_log_collector_t2")
    assert "tests/host/nrf_log_collector_test.c" in collector.get("sources", [])
    assert '"fault record event collection", test_fault_record_words_are_collected_without_decoding' in collector_test


def test_image_digest_is_prepared_before_flash_and_read_back_for_the_full_region():
    ncs = read("nRF54L15/ncs.ps1")
    flash_start = ncs.index("function Invoke-WestFlash")
    flash_end = ncs.index("function Invoke-WestNativeTestBuild", flash_start)
    flash = ncs[flash_start:flash_end]
    prepare = flash.index("prepare --input-hex")
    actual_flash = flash.index("& west flash -d $BuildDir")
    verify = flash.index("-ImageManifest $integrityManifest")
    assert prepare < actual_flash < verify
    assert 'if ($integrityExitCode -eq 4)' in flash
    assert "dump_image {$readbackTclPath}" in read("nRF54L15/verify_rram_tail.ps1")
    verifier = read("nRF54L15/verify_rram_tail.ps1")
    assert 'Join-Path $PSScriptRoot "scripts\\nrf_image_integrity.py"' in verifier
    assert "& python $integrityScript verify --manifest $ImageManifest" in verifier
