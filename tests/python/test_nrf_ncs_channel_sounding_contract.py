from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CS_CONF = ROOT / "nRF54L15" / "ncs" / "channel_sounding.conf"
NCS_PS1 = ROOT / "nRF54L15" / "ncs.ps1"
BASE_CONF = ROOT / "nRF54L15" / "zephyr" / "prj.conf"
OVERLAY = ROOT / "nRF54L15" / "zephyr" / "app.overlay"
PROTOCOL = ROOT / "nRF54L15" / "include" / "nrf_upstream_protocol.h"
DOC = ROOT / "docs" / "nrf54l15-ncs-channel-sounding-proof.md"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def config_lines(path: Path) -> set[str]:
    return {
        line.strip()
        for line in text(path).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def test_production_is_softdevice_channel_sounding_and_max45_overlay_is_controller_only():
    base = config_lines(BASE_CONF)
    for required in (
        "CONFIG_BT=y",
        "CONFIG_BT_LL_SOFTDEVICE=y",
        "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_CTLR_CHANNEL_SOUNDING=y",
        "CONFIG_BT_RAS=y",
        "CONFIG_BT_RAS_RREQ=y",
        "CONFIG_BT_RAS_RRSP=y",
        "CONFIG_BT_CS_DE=y",
    ):
        assert required in base

    proof = config_lines(CS_CONF)
    assert "CONFIG_BT_CHANNEL_SOUNDING=y" in proof
    assert "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y" in proof
    proof_raw = text(CS_CONF)
    assert "CONFIG_BT_RAS=" not in proof_raw
    assert "CONFIG_BT_CS_DE=" not in proof_raw


def test_common_production_image_has_both_ble_and_cs_roles_but_one_connection():
    cfg = config_lines(BASE_CONF)
    for line in (
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=1",
        "CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_CTLR_SDC_CS_COUNT=1",
        "CONFIG_BT_RAS_RREQ_MAX_ACTIVE_CONN=1",
        "CONFIG_BT_RAS_RRSP_MAX_ACTIVE_CONN=1",
    ):
        assert line in cfg


def test_first_production_cs_profile_is_single_antenna_no_mode3_and_has_ras_de():
    cfg = config_lines(BASE_CONF)
    for line in (
        "CONFIG_BT_CTLR_SDC_CS_MAX_ANTENNA_PATHS=1",
        "CONFIG_BT_CTLR_SDC_CS_NUM_ANTENNAS=1",
        "CONFIG_BT_RAS_MAX_ANTENNA_PATHS=1",
        "CONFIG_BT_CS_DE_MAX_NUM_ANTENNA_PATHS=1",
        "CONFIG_BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_CNT=1",
        "CONFIG_BT_CHANNEL_SOUNDING_REASSEMBLY_BUFFER_SIZE=1920",
        "CONFIG_BT_CTLR_CHANNEL_SOUNDING_TEST=n",
        "CONFIG_BT_CTLR_SDC_CS_STEP_MODE3=n",
        "CONFIG_BT_RAS_MODE_3_SUPPORTED=n",
        "CONFIG_BT_RAS=y",
        "CONFIG_BT_CS_DE=y",
    ):
        assert line in cfg


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


def test_wrapper_self_bootstraps_pinned_sdk_when_west_is_absent():
    script = text(NCS_PS1)
    assert "function Invoke-InPinnedToolchain" in script
    assert '"sdk-manager", "toolchain", "launch"' in script
    assert '"--ncs-version", $NcsVersion' in script
    assert "$PSCommandPath" in script
    assert 'if (-not (Get-Command "west" -ErrorAction SilentlyContinue))' in script


def test_wrapper_has_sdk_manager_fallback_for_repo_outside_west_workspace():
    script = text(NCS_PS1)
    assert "function Invoke-SdkManager" in script
    assert 'Invoke-SdkManager @("config", "show")' in script
    assert 'Invoke-SdkManager @("list", "--all-fields")' in script
    assert "function Get-SdkManagerInstallRoot" in script
    assert "function Get-SdkManagerSdkDirectory" in script
    assert "function Get-NcsTopDirFromSdkManager" in script
    assert "return Get-NcsTopDirFromSdkManager" in script

    env_pos = script.index("foreach ($base in @($env:NRF_BASE, $env:ZEPHYR_BASE))")
    west_pos = script.index("$topDirOutput = @(& west topdir")
    sdk_manager_pos = script.index("return Get-NcsTopDirFromSdkManager")
    assert env_pos < west_pos < sdk_manager_pos


def test_wrapper_treats_failed_west_topdir_as_probe_miss_on_windows_powershell():
    script = text(NCS_PS1)
    assert "$topDir = $null" in script
    assert "$topDirOutput = @(& west topdir 2>$null)" in script
    assert "if ($LASTEXITCODE -eq 0 -and $topDirOutput.Count -gt 0)" in script
    assert "catch {" in script
    assert "return Get-NcsTopDirFromSdkManager" in script


def test_wrapper_handles_locked_toolchain_nrfutil_home_and_current_columns():
    script = text(NCS_PS1)
    assert "$originalHome = $env:NRFUTIL_HOME" in script
    assert 'Join-Path $originalHome "locked"' in script
    assert 'Join-Path $env:USERPROFILE ".nrfutil"' in script
    assert "$env:NRFUTIL_HOME = $userHome" in script
    assert "$env:NRFUTIL_HOME = $originalHome" in script
    assert "SDK Type | SDK Version | SDK Status | SDK Directory | Toolchain Status" in script
    assert '"^\\s*nrf\\s+$versionPattern\\s+Installed' in script


def test_wrapper_honors_custom_sdk_install_dir_and_platform_default():
    script = text(NCS_PS1)
    assert "'^\\s*install-dir:\\s*(.+?)\\s*$'" in script
    assert '$configured -ne "unset"' in script
    assert '$env:OS -eq "Windows_NT"' in script
    assert 'Join-Path $systemDrive "ncs"' in script
    assert 'return "/opt/nordic/ncs"' in script
    assert 'Join-Path $HOME "ncs"' in script


def test_wrapper_reports_missing_sdk_and_rejects_wrong_version():
    script = text(NCS_PS1)
    assert "NCS $NcsVersion SDK is not installed according to 'nrfutil sdk-manager list --all-fields'." in script
    assert "nrfutil reports NCS $NcsVersion at '$candidate', but that directory is not a west workspace" in script
    assert "if ($actual -ne $NcsVersion)" in script
    assert "nRF54L15 production is pinned to $NcsVersion" in script
    assert "Assert-NcsBaseline $topDir" in script


def test_production_and_max45_proof_are_separate_actions():
    script = text(NCS_PS1)
    assert '$app = Join-Path $project "zephyr"' in script
    assert '$productionBuild = Join-Path $buildRoot "nrf54l15-production-v3.4.0"' in script
    assert '$csProofBuild = Join-Path $buildRoot "nrf54l15-cs-proof-v3.4.0"' in script
    assert '"build" {' in script
    assert "Invoke-WestBuild $productionBuild $false" in script
    assert '"cs-proof" {' in script
    assert "Invoke-WestBuild $csProofBuild $true" in script
    assert "-DEXTRA_CONF_FILE=$csConf" in script


def test_cs_proof_still_checks_resolved_controller_kconfig():
    script = text(NCS_PS1)
    for required in (
        "CONFIG_BT_MAX_CONN=1",
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


def test_upstream_wire_sizes_are_not_changed_by_max51():
    protocol = text(PROTOCOL)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH\s+12U", protocol)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH\s+102U", protocol)


def test_proof_doc_preserves_sdk_one_connection_and_historical_evidence():
    doc = text(DOC)
    assert "v3.4.0" in doc
    assert "CONFIG_BT_MAX_CONN=1" in doc
    assert "cs-proof" in doc
    assert "production" in doc.lower()
    assert "216244" in doc
    assert "43800" in doc


def test_wrapper_exposes_explicit_flash_but_proof_action_never_flashes():
    script = text(NCS_PS1)
    assert 'ValidateSet("build", "flash", "monitor", "cs-proof", "compare", "probe", "clean")' in script
    assert "function Invoke-WestFlash" in script
    proof = script[script.index('"cs-proof" {'):script.index('"compare" {')]
    assert "Invoke-WestFlash" not in proof
