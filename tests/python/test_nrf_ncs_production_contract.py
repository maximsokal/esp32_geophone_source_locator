from pathlib import Path
import json
import re


ROOT = Path(__file__).resolve().parents[2]
NCS = ROOT / "nRF54L15" / "ncs.ps1"
PIO = ROOT / "nRF54L15" / "pio.ps1"
LOCAL_VERIFY = ROOT / "tools" / "run_local_verify.py"
RUN_SUITE = ROOT / "tools" / "run_suite.ps1"
TASKS = ROOT / ".vscode" / "tasks.json"
README = ROOT / "nRF54L15" / "README.md"
ARCHITECTURE = ROOT / "ARCHITECTURE.md"
LOCAL_DOC = ROOT / "docs" / "local-verification.md"
EVIDENCE = ROOT / "docs" / "verification-evidence-matrix.md"
TEST_TASK = ROOT / "docs" / "nrf54l15-test-task.md"
BASE_CONF = ROOT / "nRF54L15" / "zephyr" / "prj.conf"
PROTOCOL = ROOT / "nRF54L15" / "include" / "nrf_upstream_protocol.h"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def config_lines(path: Path) -> set[str]:
    return {
        line.strip()
        for line in text(path).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def test_ncs_wrapper_is_canonical_production_authority_and_self_bootstraps():
    source = text(NCS)
    assert '$NcsVersion = "v3.4.0"' in source
    assert '$board = "xiao_nrf54l15/nrf54l15/cpuapp"' in source
    assert '"nrf54l15-production-v3.4.0"' in source
    assert 'ValidateSet("build", "flash", "monitor", "cs-proof", "compare", "probe", "clean")' in source
    assert "function Invoke-InPinnedToolchain" in source
    assert '"sdk-manager", "toolchain", "launch"' in source
    assert '"--ncs-version", $NcsVersion' in source
    assert 'if (-not (Get-Command "west"' in source
    assert "Invoke-InPinnedToolchain" in source


def test_production_build_uses_permanent_max51_channel_sounding_config():
    source = text(NCS)
    build_case = source[source.index('"build" {'):source.index('"flash" {')]
    proof_case = source[source.index('"cs-proof" {'):source.index('"compare" {')]

    assert "Invoke-WestBuild $productionBuild $false" in build_case
    assert "$csConf" not in build_case
    assert "Probe-ProductionBuild" in build_case
    assert "Invoke-WestBuild $csProofBuild $true" in proof_case
    assert "Probe-CsBuild" in proof_case

    cfg = config_lines(BASE_CONF)
    for required in (
        "CONFIG_BT=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=1",
        "CONFIG_BT_LL_SOFTDEVICE=y",
        "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_RAS=y",
        "CONFIG_BT_RAS_RREQ=y",
        "CONFIG_BT_RAS_RRSP=y",
        "CONFIG_BT_CS_DE=y",
    ):
        assert required in cfg


def test_canonical_flash_is_explicit_ncs_hardware_action():
    source = text(NCS)
    flash_case = source[source.index('"flash" {'):source.index('"cs-proof" {')]
    assert "Invoke-WestBuild $productionBuild $false" in flash_case
    assert "Probe-ProductionBuild" in flash_case
    assert "Invoke-WestFlash $productionBuild" in flash_case
    assert "west flash -d $BuildDir" in source


def test_local_verify_routes_nrf_build_to_ncs_not_platformio():
    source = text(LOCAL_VERIFY)
    nrf_start = source.index("def nrf_steps(")
    nrf_end = source.index("def scope_includes", nrf_start)
    nrf = source[nrf_start:nrf_end]
    assert 'root / "nRF54L15" / "ncs.ps1"' in nrf
    assert '"-Action", "build"' in nrf
    assert "pio.ps1" not in nrf
    assert "PlatformIO compatibility tests are intentionally" in nrf


def test_run_all_routes_nrf_build_to_ncs_not_platformio():
    source = text(RUN_SUITE)
    assert '$nrfNcsWrapper = Join-Path $repo "nRF54L15\\ncs.ps1"' in source
    assert 'Invoke-SuiteStep -Name "nRF54L15 NCS production build"' in source
    assert '"-Action", "build"' in source
    all_branch = source[source.index('if ($Suite -eq "All") {'):]
    assert "pio.ps1" not in all_branch
    assert "PlatformIO compatibility tests are excluded" in source


def test_vscode_build_flash_and_monitor_use_ncs_and_pio_is_named_compatibility():
    document = json.loads(text(TASKS))
    by_label = {task["label"]: task for task in document["tasks"]}

    build = by_label["Build: nRF54L15 production (NCS)"]
    flash = by_label["nRF54L15: Flash firmware"]
    monitor = by_label["nRF54L15: Monitor logs"]
    compatibility = by_label["nRF54L15: PlatformIO compatibility build"]

    for task in (build, flash, monitor):
        joined = " ".join(map(str, task["args"]))
        assert "nRF54L15/ncs.ps1" in joined or "nRF54L15\\ncs.ps1" in joined
        assert "pio.ps1" not in joined

    assert build["args"][build["args"].index("-Action") + 1] == "build"
    assert flash["args"][flash["args"].index("-Action") + 1] == "flash"
    assert monitor["args"][monitor["args"].index("-Action") + 1] == "monitor"
    assert "${input:nrfMonitorPort}" in monitor["args"]

    compatibility_joined = " ".join(map(str, compatibility["args"]))
    assert "pio.ps1" in compatibility_joined
    assert "compatibility" in compatibility["label"].lower()
    assert "not the production firmware" in compatibility["detail"].lower()


def test_docs_consistently_name_ncs_340_as_canonical_and_platformio_as_compatibility():
    for path in (README, ARCHITECTURE, LOCAL_DOC, EVIDENCE, TEST_TASK):
        source = text(path)
        lowered = source.lower()
        assert "ncs v3.4.0" in lowered or "nrf connect sdk v3.4.0" in lowered
        assert "canonical" in lowered
        assert "platformio" in lowered
        assert "compatibility" in lowered

    readme = text(README)
    lowered_readme = readme.lower()
    assert "pio.ps1 upload" in readme
    compatibility_section = lowered_readme[lowered_readme.index("## platformio compatibility path"):]
    assert "production build/flash authority" in compatibility_section
    assert "not" in compatibility_section[:400]

    architecture = text(ARCHITECTURE)
    assert "canonical nrf54l15 production/flash toolchain" in architecture.lower()
    assert "one production PlatformIO environment" not in architecture
    assert "current Seeed PlatformIO + Zephyr" not in architecture
    assert ".\\nRF54L15\\ncs.ps1 build" in architecture
    assert ".\\nRF54L15\\ncs.ps1 flash" in architecture
    assert "NCS v3.4.0 /\nZephyr 4.4.0 source" in architecture


def test_platformio_wrapper_still_exists_but_is_not_used_by_canonical_runners():
    assert PIO.is_file()
    assert "pio.ps1" not in text(LOCAL_VERIFY)[
        text(LOCAL_VERIFY).index("def nrf_steps("):
        text(LOCAL_VERIFY).index("def scope_includes")
    ]
    assert 'nRF54L15\\pio.ps1' not in text(RUN_SUITE)


def test_wire_contract_stays_12_and_112_bytes():
    protocol = text(PROTOCOL)
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH\s+12U", protocol)
    # 112U as of MAX-114 (altitude/orientation fields appended); this guard
    # is about the NCS production promotion not touching the sensor
    # snapshot frame, not about the frame being frozen at its previous size.
    assert re.search(r"NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH\s+112U", protocol)
