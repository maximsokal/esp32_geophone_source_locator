from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[2]
PAIR_H = ROOT / "nRF54L15" / "include" / "nrf_cs_pair_adapter.h"
PAIR_C = ROOT / "nRF54L15" / "src" / "nrf_cs_pair_adapter.c"
RUNTIME_H = ROOT / "nRF54L15" / "include" / "nrf_cs_pair_runtime_zephyr.h"
RUNTIME_C = ROOT / "nRF54L15" / "src" / "nrf_cs_pair_runtime_zephyr.c"
RUNTIME_PARTS = [
    ROOT / "nRF54L15" / "src" / f"nrf_cs_pair_runtime_zephyr_impl_{index}.inc"
    for index in (1, 2, 3)
]
RUNTIME_PARTS.append(ROOT / "nRF54L15" / "src" / "nrf_cs_pair_runtime_zephyr_connections.inc")
SCHED_H = ROOT / "nRF54L15" / "include" / "nrf_cs_scheduler.h"
BOOTSTRAP = ROOT / "nRF54L15" / "src" / "nrf_bootstrap_runtime_zephyr.c"
MAIN = ROOT / "nRF54L15" / "src" / "main.c"
CMAKE = ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt"
CONF = ROOT / "nRF54L15" / "zephyr" / "prj.conf"
PROTOCOL = ROOT / "nRF54L15" / "include" / "nrf_upstream_protocol.h"
MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "coverage-nrf.json"
HOST_TEST = ROOT / "tests" / "host" / "nrf_cs_pair_adapter_test.c"
DOC = ROOT / "docs" / "nrf54l15-channel-sounding-pair-runtime.md"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def runtime_source() -> str:
    return text(RUNTIME_C) + "\n" + "\n".join(text(path) for path in RUNTIME_PARTS)


def config_lines(path: Path) -> set[str]:
    return {
        line.strip()
        for line in text(path).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def test_pair_adapter_core_is_platform_neutral_and_token_guarded():
    header = text(PAIR_H)
    source = text(PAIR_C)
    lowered = (header + source).lower()

    assert '#include "nrf_cs_scheduler.h"' in header
    assert "cycle_id" not in header or "nrf_cs_step_t" in header
    assert "step_matches" in source
    assert "membership_generation" in source
    assert "attempt_id" in source
    assert "NRF_CS_PAIR_ADAPTER_ERR_STALE" in source
    assert "zephyr/" not in lowered
    assert "bluetooth/" not in lowered
    assert "k_sem" not in lowered
    assert "k_work" not in lowered


def test_pair_adapter_has_bounded_cleanup_before_completion():
    source = text(PAIR_C)
    assert "NRF_CS_PAIR_PHASE_CLEANUP" in source
    assert "terminal_pending" in source
    assert "adapter->ops.cleanup" in source
    cleanup_pos = source.index("nrf_cs_pair_adapter_cleanup_complete")
    assert "callback(&completed_step, &completed_measurement" in source[cleanup_pos:]
    assert "NRF_CS_OUTCOME_TIMEOUT" in source
    assert "NRF_CS_OUTCOME_TRANSPORT_ERROR" in source


def test_runtime_is_common_dual_role_ras_cs_image_with_one_connection():
    cfg = config_lines(CONF)
    for required in (
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=1",
        "CONFIG_BT_LL_SOFTDEVICE=y",
        "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_CTLR_SDC_CS_COUNT=1",
        "CONFIG_BT_RAS=y",
        "CONFIG_BT_RAS_RREQ=y",
        "CONFIG_BT_RAS_RRSP=y",
        "CONFIG_BT_RAS_RRSP_AUTO_ALLOC_INSTANCE=y",
        "CONFIG_BT_CS_DE=y",
        "CONFIG_BT_CTLR_SDC_CS_MAX_ANTENNA_PATHS=1",
        "CONFIG_BT_CTLR_SDC_CS_NUM_ANTENNAS=1",
        "CONFIG_BT_RAS_MODE_3_SUPPORTED=n",
        "CONFIG_BT_CTLR_SDC_CS_STEP_MODE3=n",
    ):
        assert required in cfg


def test_runtime_uses_exact_ncs_ras_cs_sequence_without_unbounded_or_reboot_orchestration():
    source = runtime_source()

    for api in (
        "bt_enable",
        "bt_le_scan_start",
        "bt_conn_le_create",
        "bt_conn_set_security",
        "bt_gatt_exchange_mtu",
        "bt_gatt_dm_start",
        "bt_ras_rreq_alloc_and_assign_handles",
        "bt_ras_rreq_read_features",
        "bt_ras_rreq_realtime_rd_subscribe",
        "bt_ras_rreq_on_demand_rd_subscribe",
        "bt_le_cs_read_remote_supported_capabilities",
        "bt_le_cs_create_config",
        "bt_le_cs_security_enable",
        "bt_le_cs_set_procedure_parameters",
        "bt_le_cs_procedure_enable",
        "bt_ras_rreq_rd_subevent_data_parse",
        "cs_de_calc",
    ):
        assert api in source

    assert "K_FOREVER" not in source
    assert "sys_reboot" not in source
    assert ".max_procedure_count = 1U" not in source
    assert "params->max_procedure_count = 1U;" in source
    assert "reschedule_cs_work(&s_timeout_work" in source


def test_runtime_maps_node_assignment_to_advertised_identity_and_keeps_wired_role_separate():
    bootstrap = text(BOOTSTRAP)
    runtime = runtime_source()
    header = text(RUNTIME_H)

    # Assignment completion uses a pointer to the committed (or retried) ASSIGN.
    assert "nrf_cs_pair_runtime_zephyr_set_node_id(assign->node_id)" in bootstrap
    assert bootstrap.index("nrf_cs_pair_runtime_zephyr_set_node_id(assign->node_id)") < bootstrap.index(
        "publish_core_status();", bootstrap.index("static void complete_committed_assignment")
    )
    assert re.search(r"assign\.role\s*==\s*NRF_UPSTREAM_ROLE_REMOTE", bootstrap)
    assert "nrf_cs_pair_runtime_zephyr_set_node_id" in header
    assert "BT_DATA_SVC_DATA16" in runtime
    assert "BT_UUID_RANGING_SERVICE_VAL" in runtime
    assert "s_service_data[2] = node_id" in runtime
    assert "step->initiator != s_node_id" in runtime
    assert "NRF_NODE_ROLE_MAIN" not in runtime
    assert "NRF_NODE_ROLE_REMOTE" not in runtime


def test_runtime_executes_one_directed_attempt_and_leaves_multi_node_authority_out_of_scope():
    source = runtime_source()
    header = text(RUNTIME_H)
    scheduler_header = text(SCHED_H)

    assert "nrf_cs_pair_runtime_zephyr_execute" in header
    assert "const nrf_cs_step_t *step" in header
    assert "step->initiator != s_node_id" in source
    assert "nrf_cs_scheduler_start_cycle" not in source
    assert "nrf_cs_scheduler_next_step" not in source
    assert "nrf_cs_scheduler_complete_step" not in source
    assert "runtime_step_matches_active" in source
    assert "event.has_step && !runtime_step_matches_active(&event.step)" in source
    assert "NRF_CS_MAX_NODES" in scheduler_header


def test_distance_confidence_comes_from_measurement_evidence():
    """MAX-206: a success no longer reports a constant 1000; confidence is
    derived from tone coverage and PBR/RTT agreement by a pure, host-tested
    function (nrf_cs_measurement_quality.c)."""
    runtime = runtime_source()
    header = text(RUNTIME_H)

    assert "CS_DE_QUALITY_OK" in runtime
    assert "distance_estimates[0].best" in runtime
    assert "isfinite(best)" in runtime
    assert "measurement.confidence = 1000U" not in runtime
    assert "measurement.confidence = nrf_cs_measurement_confidence_with_peak(" in runtime
    assert "peak_floor_ratio" in runtime      # the sharpness of the IFFT peak is an input of the confidence (bench 2026-11-03)
    assert "good_tones" in runtime
    assert "distance_estimates[0].rtt" in runtime
    assert "NRF_CS_OUTCOME_OUTLIER" in runtime
    assert "NRF_CS_PROVIDER_QUALITY_TONE_OK" in header
    assert "NRF_CS_PROVIDER_QUALITY_PBR_VALID" in header
    assert "NRF_CS_PROVIDER_QUALITY_RTT_VALID" in header


def test_common_build_and_host_manifest_include_pair_adapter():
    cmake = text(CMAKE)
    manifest = json.loads(text(MANIFEST))

    assert "../src/nrf_cs_pair_adapter.c" in cmake
    assert "../src/nrf_cs_pair_runtime_zephyr.c" in cmake
    wrapper = text(RUNTIME_C)
    for index in (1, 2, 3):
        assert f'#include "nrf_cs_pair_runtime_zephyr_impl_{index}.inc"' in wrapper
    assert "if(CONFIG_BT_LL_SOFTDEVICE)" in cmake
    assert "MAX-51 requires Central+Peripheral+CS+RAS RREQ/RRSP+CS_DE" in cmake
    assert "MAX-51 requires one serialized BLE/CS/RAS connection context" in cmake

    names = {entry["name"] for entry in manifest["tests"]}
    assert "nrf_cs_pair_adapter" in names
    entry = next(entry for entry in manifest["tests"] if entry["name"] == "nrf_cs_pair_adapter")
    assert "nRF54L15/src/nrf_cs_pair_adapter.c" in entry["sources"]
    assert "tests/host/nrf_cs_pair_adapter_test.c" in entry["sources"]
    assert HOST_TEST.is_file()


def test_main_starts_bluetooth_pair_runtime_before_assignment_processing():
    source = text(MAIN)
    cs_pos = source.index("nrf_cs_pair_runtime_zephyr_start")
    bootstrap_pos = source.index("nrf_bootstrap_runtime_zephyr_start")
    assert cs_pos < bootstrap_pos


def test_existing_upstream_wire_contract_is_unchanged():
    protocol = text(PROTOCOL)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH\s+12U", protocol)
    # Preserve v1 decode compatibility while keeping later extensions versioned.
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH\s+112U", protocol)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V2_LENGTH\s+120U", protocol)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH\s+136U", protocol)
    assert re.search(
        r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH\s+NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH",
        protocol,
    )


def test_max51_document_states_hil_and_max97_boundaries():
    doc = text(DOC).lower()
    assert "max-51" in doc
    assert "max-97" in doc
    assert "bt_max_conn=1" in doc
    assert "hil" in doc
    assert "accuracy" in doc
    assert "one directed pair" in doc
