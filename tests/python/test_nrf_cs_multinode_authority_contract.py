from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[2]
CONTROL_H = ROOT / "nRF54L15" / "include" / "nrf_cs_control_protocol.h"
CONTROL_C = ROOT / "nRF54L15" / "src" / "nrf_cs_control_protocol.c"
CONTROL_RUNTIME = ROOT / "nRF54L15" / "src" / "nrf_cs_control_runtime.c"
BOOTSTRAP = ROOT / "nRF54L15" / "src" / "nrf_bootstrap_runtime_zephyr.c"
NCS_CMAKE = ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt"
UPSTREAM_H = ROOT / "nRF54L15" / "include" / "nrf_upstream_protocol.h"
AUTH_H = ROOT / "components" / "nrf_cs_authority" / "include" / "nrf_cs_authority.h"
AUTH_C = ROOT / "components" / "nrf_cs_authority" / "nrf_cs_authority.c"
TOPO_C = ROOT / "components" / "nrf_topology_service" / "nrf_topology_service.c"
TOPO_CMAKE = ROOT / "components" / "nrf_topology_service" / "CMakeLists.txt"
TOPO_H = ROOT / "components" / "nrf_topology_service" / "include" / "nrf_topology_service.h"
SHARED_CMAKE = ROOT / "components" / "nrf_cs_shared" / "CMakeLists.txt"
MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "coverage-nrf.json"
DOC = ROOT / "docs" / "nrf54l15-channel-sounding-multinode-authority.md"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_control_plane_is_versioned_and_legacy_sensor_frames_are_unchanged():
    control = text(CONTROL_H)
    upstream = text(UPSTREAM_H)
    assert re.search(r"NRF_CS_CONTROL_EXECUTE_LENGTH\s+32U", control)
    assert re.search(r"NRF_CS_CONTROL_STATUS_REQUEST_LENGTH\s+32U", control)
    assert re.search(r"NRF_CS_CONTROL_RESULT_LENGTH\s+44U", control)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH\s+12U", upstream)
    # Preserve v1 decode compatibility while keeping later extensions versioned.
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH\s+112U", upstream)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V2_LENGTH\s+120U", upstream)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH\s+136U", upstream)
    assert re.search(
        r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH\s+NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH",
        upstream,
    )
    assert "NRF_UPSTREAM_PROTOCOL_VERSION" in text(CONTROL_C)
    assert "nrf_upstream_protocol_crc16_ccitt_false" in text(CONTROL_C)


def test_full_scheduler_token_crosses_i2c_and_stale_results_are_rejected():
    source = text(CONTROL_C) + text(CONTROL_RUNTIME) + text(AUTH_C)
    for token in (
        "cycle_id",
        "membership_generation",
        "attempt_id",
        "pair_index",
        "attempt_number",
        "initiator",
        "reflector",
        "timeout_ms",
    ):
        assert token in source
    assert "nrf_cs_control_step_equal" in source
    assert "NRF_CS_CONTROL_RESULT_STALE" in source
    assert "remote.request_id != authority->request.request_id" in text(AUTH_C)


def test_nrf_control_runtime_is_idempotent_and_publishes_busy_invalid_and_stale():
    source = text(CONTROL_RUNTIME)
    assert "same_request(request, &runtime->active_request)" in source
    assert "start_required" in source
    assert "NRF_CS_CONTROL_RESULT_BUSY" in source
    assert "NRF_CS_CONTROL_RESULT_INVALID" in source
    assert "NRF_CS_CONTROL_RESULT_STALE" in source
    assert "immediate_result" in source
    assert "immediate_valid" in text(ROOT / "nRF54L15" / "include" / "nrf_cs_control_runtime.h")


def test_bootstrap_keeps_twis_callbacks_short_and_executes_cs_in_work_queue():
    source = text(BOOTSTRAP)
    assert "K_MSGQ_DEFINE(s_cs_execute_queue" in source
    assert "k_work_submit(&s_cs_execute_work)" in source
    execute_pos = source.index("nrf_cs_pair_runtime_zephyr_execute")
    worker_pos = source.index("cs_execute_work_handler")
    assert execute_pos > worker_pos
    assert "nrf_cs_control_runtime_take_selected" in source
    assert "NRF_CS_CONTROL_RESULT_LENGTH" in source


def test_esp32_is_single_authority_and_reuses_max49_scheduler_in_existing_topology_worker():
    authority = text(AUTH_C)
    topology = text(TOPO_C)
    cmake = text(TOPO_CMAKE)
    header = text(TOPO_H)
    assert "nrf_cs_scheduler_start_cycle" in authority
    assert "nrf_cs_scheduler_next_step" in authority
    assert "nrf_cs_scheduler_complete_step" in authority
    assert '#include "nrf_topology_service.c"' not in topology
    assert "service_worker_pre_max97" not in topology
    assert "nrf_topology_service_max97.c" not in cmake
    assert '"nrf_topology_service.c"' in cmake
    assert "static nrf_topology_worker_state_t s_worker_state;" in topology
    assert "uxTaskGetStackHighWaterMark(NULL)" in topology
    assert re.search(r"NRF_TOPOLOGY_STACK_MARGIN_BYTES\s+1024U", topology)
    assert "nrf_topology_service_request_cs_cycle" in header
    assert "nrf_topology_service_get_cs_cycle" in header
    assert topology.count("xTaskCreatePinnedToCore") == 1


def test_remote_to_remote_is_routed_by_stable_node_id_through_same_tca_owner():
    topology = text(TOPO_C)
    assert "request->step.initiator" in topology
    assert "snapshot->remotes[channel].node_id == node" in topology
    assert "platform_mux_select" in topology
    assert "platform_mux_close" in topology
    assert "cs_route_open" in topology
    assert "cs_route_close" in topology
    assert "service_request_topology_refresh_from_worker" in topology


def test_builds_include_shared_control_runtime_on_both_firmware_sides():
    ncs = text(NCS_CMAKE)
    shared = text(SHARED_CMAKE)
    assert "../src/nrf_cs_control_protocol.c" in ncs
    assert "../src/nrf_cs_control_runtime.c" in ncs
    assert "nrf_cs_scheduler.c" in shared
    assert "nrf_cs_control_protocol.c" in shared


def test_host_manifest_covers_codec_runtime_authority_and_bootstrap_integration():
    manifest = json.loads(text(MANIFEST))
    entries = {entry["name"]: entry for entry in manifest["tests"]}
    for name in ("nrf_cs_control_protocol", "nrf_cs_control_runtime", "nrf_cs_authority"):
        assert name in entries
    bootstrap_sources = entries["nrf_bootstrap_runtime_zephyr"]["sources"]
    assert "nRF54L15/src/nrf_cs_control_protocol.c" in bootstrap_sources
    assert "nRF54L15/src/nrf_cs_control_runtime.c" in bootstrap_sources
    assert (ROOT / "tests" / "host" / "nrf_cs_control_protocol_test.c").is_file()
    assert (ROOT / "tests" / "host" / "nrf_cs_control_runtime_test.c").is_file()
    assert (ROOT / "tests" / "host" / "nrf_cs_authority_test.c").is_file()


def test_architecture_document_names_single_authority_recovery_and_hil_boundary():
    doc = text(DOC).lower()
    for phrase in (
        "single global channel sounding cycle authority",
        "esp32-s3",
        "split brain",
        "remote-to-remote",
        "bt_max_conn=1",
        "lost i2c acknowledgement",
        "rejoin",
        "next cycle",
        "hil-only",
    ):
        assert phrase in doc
