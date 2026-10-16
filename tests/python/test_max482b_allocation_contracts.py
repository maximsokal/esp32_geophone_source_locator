import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/host/manifest.d/max482b-failure-paths.json"


def test_max482b_host_fragments_are_registered():
    tests = json.loads(MANIFEST.read_text(encoding="utf-8"))["tests"]
    by_name = {entry["name"]: entry for entry in tests}

    assert set(by_name) == {
        "max482b_wifi_platform_oom",
        "max482b_sd_dma_oom",
        "max482b_recorder_shutdown_oom",
    }
    assert "tests/host/max482b_wifi_platform_oom_test.c" in by_name[
        "max482b_wifi_platform_oom"
    ]["sources"]
    assert "tests/host/max482b_sd_dma_oom_test.c" in by_name[
        "max482b_sd_dma_oom"
    ]["sources"]
    assert "tests/host/seismic_recorder_fault_fsm_test.c" in by_name[
        "max482b_recorder_shutdown_oom"
    ]["sources"]
    assert "calloc=seismic_recorder_fault_calloc" in by_name[
        "max482b_recorder_shutdown_oom"
    ]["defines"]
    assert "realloc=seismic_recorder_fault_realloc" in by_name[
        "max482b_recorder_shutdown_oom"
    ]["defines"]


def test_wifi_oom_retry_checks_live_netif_rollback():
    test = (ROOT / "tests/host/max482b_wifi_platform_oom_test.c").read_text(encoding="utf-8")
    source = (ROOT / "components/wifi_manager/wifi_manager_platform.c").read_text(
        encoding="utf-8"
    )

    assert "s_fail_netif_create_nth = 2U;" in test
    assert "assert(live_netif_count() == 0U);" in test
    assert "assert(wifi_manager_platform_init() == ESP_OK);" in test
    assert "wifi_manager_platform_destroy_default_netifs();" in source
    assert "esp_netif_destroy_default_wifi(s_sta_netif);" in source
    assert "esp_wifi_deinit();" in source


def test_sd_dma_oom_uses_real_host_allocation_and_checks_guard_and_caps():
    test = (ROOT / "tests/host/max482b_sd_dma_oom_test.c").read_text(encoding="utf-8")
    source = (ROOT / "components/sd_card/sd_card_format.c").read_text(encoding="utf-8")

    assert "malloc(" in test and "free(header);" in test
    assert "MAX482B_TAIL_GUARD" in test
    assert "s_largest_internal_dma_block < 4096U" in test
    assert "s_psram_free_bytes > 4096U" in test
    assert "status.err == ESP_ERR_NO_MEM" in test
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT" in test
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT" in source


def test_production_heap_poisoning_stays_disabled():
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    assert "CONFIG_HEAP_POISONING_DISABLED=y" in defaults
    assert "# CONFIG_HEAP_POISONING_LIGHT is not set" in defaults
    assert "# CONFIG_HEAP_POISONING_COMPREHENSIVE is not set" in defaults


def test_recorder_shutdown_sweep_rejects_incomplete_metadata():
    test = (ROOT / "tests/host/seismic_recorder_fault_fsm_test.c").read_text(
        encoding="utf-8"
    )

    assert "measure_successful_shutdown_allocations" in test
    assert "for (size_t nth = 1U; nth <= allocation_points; ++nth)" in test
    assert "assert(snapshot.stop_stage_error == ESP_ERR_NO_MEM);" in test
    assert "test_shutdown_allocation_failures_never_complete();" in test
