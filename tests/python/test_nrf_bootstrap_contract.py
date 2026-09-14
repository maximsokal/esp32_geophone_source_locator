from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_nrf_production_image_links_bootstrap_stack():
    cmake = (ROOT / "nRF54L15/zephyr/CMakeLists.txt").read_text(encoding="utf-8")
    for source in (
        "../src/nrf_upstream_protocol.c",
        "../src/nrf_bootstrap.c",
        "../src/nrf_upstream_target_zephyr.c",
        "../src/nrf_bootstrap_runtime_zephyr.c",
    ):
        assert source in cmake


def test_nrf_bootstrap_runtime_is_started_from_single_production_main():
    main = (ROOT / "nRF54L15/src/main.c").read_text(encoding="utf-8")
    assert "nrf_bootstrap_runtime_zephyr_start(&node_state)" in main
    assert "NRF_NODE_ROLE_MAIN" not in main
    assert "NRF_NODE_ROLE_REMOTE" not in main


def test_unrecoverable_target_loss_has_reboot_recovery_enabled():
    config = (ROOT / "nRF54L15/zephyr/prj.conf").read_text(encoding="utf-8")
    runtime = (ROOT / "nRF54L15/src/nrf_bootstrap_runtime_zephyr.c").read_text(encoding="utf-8")
    assert "CONFIG_REBOOT=y" in config
    assert "sys_reboot(SYS_REBOOT_COLD)" in runtime


def test_protocol_v1_address_and_size_contract_is_explicit():
    header = (ROOT / "nRF54L15/include/nrf_upstream_protocol.h").read_text(encoding="utf-8")
    assert "NRF_UPSTREAM_ADDRESS_DISCOVERY 0x30U" in header
    assert "NRF_UPSTREAM_ADDRESS_MAIN      0x31U" in header
    assert "NRF_UPSTREAM_ADDRESS_REMOTE    0x32U" in header
    assert "NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH 24U" in header
    assert "NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH 28U" in header
