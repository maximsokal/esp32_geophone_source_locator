from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_scheduler_core_is_platform_neutral() -> None:
    header = read("nRF54L15/include/nrf_cs_scheduler.h")
    source = read("nRF54L15/src/nrf_cs_scheduler.c")
    combined = header + source

    for forbidden in (
        "<zephyr/",
        "<bluetooth/",
        "CONFIG_BT_",
        "bt_conn",
        "bt_le_",
        "nrf_upstream_protocol",
    ):
        assert forbidden not in combined


def test_scheduler_bounds_match_five_node_project_model() -> None:
    header = read("nRF54L15/include/nrf_cs_scheduler.h")
    assert "#define NRF_CS_MAX_NODES 5U" in header
    assert "#define NRF_CS_MAX_PAIRS 10U" in header
    assert "#define NRF_CS_MAX_EXECUTIONS (NRF_CS_MAX_NODES * (NRF_CS_MAX_NODES - 1U))" in header
    assert 5 * (5 - 1) == 20  # all directed executions in the five-node model
    assert "NRF_CS_PAIR_MISSING" in header
    assert "NRF_CS_PAIR_RETRY_EXHAUSTED" in header
    assert "uint32_t membership_generation;" in header
    assert "uint32_t attempt_id;" in header


def test_two_node_cycle_keeps_one_matrix_pair_but_two_directions() -> None:
    source = read("nRF54L15/src/nrf_cs_scheduler.c")
    marker = "if (scheduler->node_count == 2U)"
    start = source.index(marker)
    block = source[start:source.index("return;", start) + len("return;")]
    assert "left, right" in block
    assert "right, left" in block


def test_scheduler_is_built_and_host_covered() -> None:
    cmake = read("nRF54L15/zephyr/CMakeLists.txt")
    manifest = read("tests/host/manifest.d/coverage-nrf.json")
    assert "../src/nrf_cs_scheduler.c" in cmake
    assert '"name": "nrf_cs_scheduler"' in manifest
    assert "nRF54L15/src/nrf_cs_scheduler.c" in manifest
    assert "tests/host/nrf_cs_scheduler_test.c" in manifest


def test_existing_upstream_wire_lengths_remain_unchanged() -> None:
    protocol = read("nRF54L15/include/nrf_upstream_protocol.h")
    assert "#define NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH 12U" in protocol
    # Preserve v1 while later extensions define the current frame.
    assert "#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH 112U" in protocol
    assert "#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V2_LENGTH 120U" in protocol
    assert "#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH 136U" in protocol
    assert "#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH" in protocol
