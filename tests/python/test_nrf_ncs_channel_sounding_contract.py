from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CS_CONF = ROOT / "nRF54L15" / "ncs" / "channel_sounding.conf"
NCS_PS1 = ROOT / "nRF54L15" / "ncs.ps1"
BASE_CONF = ROOT / "nRF54L15" / "zephyr" / "prj.conf"
OVERLAY = ROOT / "nRF54L15" / "zephyr" / "app.overlay"
PROTOCOL = ROOT / "nRF54L15" / "include" / "nrf_upstream_protocol.h"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def config_lines(path: Path) -> set[str]:
    return {
        line.strip()
        for line in text(path).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def test_ncs_proof_is_additive_and_softdevice_backed():
    cfg = config_lines(CS_CONF)
    assert "CONFIG_BT=y" in cfg
    assert "CONFIG_BT_LL_SOFTDEVICE=y" in cfg
    assert "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y" in cfg
    assert "CONFIG_BT_CHANNEL_SOUNDING=y" in cfg
    assert "CONFIG_BT_CTLR_CHANNEL_SOUNDING=y" in cfg

    base = text(BASE_CONF)
    assert "CONFIG_BT_CHANNEL_SOUNDING" not in base
    assert "CONFIG_BT_LL_SOFTDEVICE" not in base


def test_common_image_has_both_ble_and_cs_roles_but_one_cs_context():
    cfg = config_lines(CS_CONF)
    for line in (
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=2",
        "CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_CTLR_SDC_CS_COUNT=1",
    ):
        assert line in cfg


def test_first_cs_profile_is_single_antenna_and_no_mode3_or_test_commands():
    raw = text(CS_CONF)
    cfg = config_lines(CS_CONF)
    assert "CONFIG_BT_CTLR_SDC_CS_MAX_ANTENNA_PATHS=1" in cfg
    assert "CONFIG_BT_CTLR_SDC_CS_NUM_ANTENNAS=1" in cfg
    assert "CONFIG_BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_CNT=1" in cfg
    assert "CONFIG_BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_SIZE=1920" in cfg
    assert "CONFIG_BT_CTLR_CHANNEL_SOUNDING_TEST=n" in cfg
    assert "CONFIG_BT_CTLR_SDC_CS_STEP_MODE3=n" in cfg
    assert "CONFIG_BT_RAS=" not in raw
    assert "CONFIG_BT_CS_DE=" not in raw


def test_wrapper_is_pinned_to_ncs_340_and_upstream_xiao_target():
    script = text(NCS_PS1)
    assert '$NcsVersion = "v3.4.0"' in script
    assert '[string]$NcsVersion' not in script
    assert '$board = "xiao_nrf54l15/nrf54l15/cpuapp"' in script
    assert "$env:NRF_BASE" in script
    assert "$env:ZEPHYR_BASE" in script
    assert "west topdir" in script
    assert "nrf\\VERSION" in script
    assert "platform-seeedboards" not in script
    assert "BOARD_ROOT" not in script


def test_wrapper_builds_same_canonical_zephyr_app_and_checks_resolved_kconfig():
    script = text(NCS_PS1)
    assert '$app = Join-Path $project "zephyr"' in script
    assert "-DEXTRA_CONF_FILE=$csConf" in script
    for required in (
        "CONFIG_BT_LL_SOFTDEVICE=y",
        "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_CTLR_SDC_CS_COUNT=1",
        "CONFIG_I2C_NRFX_TWIS_BUF_SIZE=128",
    ):
        assert required in script


def test_existing_dual_i2c_overlay_contract_is_untouched():
    overlay = text(OVERLAY)
    assert "&i2c22" in overlay
    assert 'compatible = "nordic,nrf-twis";' in overlay
    assert "NRF_PSEL(TWIS_SDA, 1, 10)" in overlay
    assert "NRF_PSEL(TWIS_SCL, 1, 11)" in overlay
    assert "&lsm6ds3tr_c" in overlay
    assert 'status = "disabled";' in overlay
    assert "&i2c30" not in overlay or "nordic,nrf-twis" not in overlay.split("&i2c30", 1)[-1]


def test_upstream_wire_sizes_are_not_changed_by_ncs_proof():
    protocol = text(PROTOCOL)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH\s+12U", protocol)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH\s+102U", protocol)


def test_wrapper_is_build_only_and_never_flashes():
    script = text(NCS_PS1).lower()
    assert 'validateset("build", "compare", "probe", "clean")' in script
    assert "west flash" not in script
    assert "nrfjprog" not in script
    assert "upload" not in script
