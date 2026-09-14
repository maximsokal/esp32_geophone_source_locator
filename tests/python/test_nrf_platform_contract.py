from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = REPO_ROOT / "nRF54L15" / "platformio.ini"
README = REPO_ROOT / "nRF54L15" / "README.md"
TEST_TASK = REPO_ROOT / "docs" / "nrf54l15-test-task.md"
CMAKE = REPO_ROOT / "nRF54L15" / "zephyr" / "CMakeLists.txt"
PRJ_CONF = REPO_ROOT / "nRF54L15" / "zephyr" / "prj.conf"
APP_OVERLAY = REPO_ROOT / "nRF54L15" / "zephyr" / "app.overlay"
MAIN = REPO_ROOT / "nRF54L15" / "src" / "main.c"
SENSOR_BUS_ZEPHYR = REPO_ROOT / "nRF54L15" / "src" / "nrf_sensor_bus_zephyr.c"
VERIFIED_SEEED_PLATFORM = "1ec1287f8e4bc4067a6fd593991e36875aef989f"


def test_nrf_production_platform_is_commit_pinned_and_single_environment():
    source = PLATFORMIO.read_text(encoding="utf-8")

    environments = re.findall(r"(?m)^\[env:([^\]]+)\]$", source)
    assert environments == ["seeed-xiao-nrf54l15"]
    assert (
        "platform = https://github.com/Seeed-Studio/platform-seeedboards.git#"
        + VERIFIED_SEEED_PLATFORM
    ) in source
    assert "framework = zephyr" in source
    assert "board = seeed-xiao-nrf54l15" in source
    assert "sense" not in source.lower()


def test_nrf_zephyr_target_compiles_runtime_policy_and_sensor_pipeline_sources():
    source = CMAKE.read_text(encoding="utf-8")

    assert 'set(EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay")' in source
    assert "../src/main.c" in source
    assert "../src/nrf_node_state.c" in source
    assert "../src/nrf_sensor_bus.c" in source
    assert "../src/nrf_sensor_bus_zephyr.c" in source
    assert "../src/nrf_sensor_snapshot.c" in source
    assert "target_include_directories(app PRIVATE ../include)" in source


def test_plain_xiao_overlay_disables_phantom_sense_imu_but_keeps_private_bus():
    source = APP_OVERLAY.read_text(encoding="utf-8")

    assert "/delete-property/ imu0;" in source
    assert "&lsm6ds3tr_c" in source
    assert 'status = "disabled";' in source
    assert "&i2c30" not in source
    assert "&i2c22" not in source


def test_private_sensor_bus_uses_i2c30_only_and_i2c_is_explicitly_enabled():
    source = SENSOR_BUS_ZEPHYR.read_text(encoding="utf-8")
    config = PRJ_CONF.read_text(encoding="utf-8")

    assert "DT_NODELABEL(i2c30)" in source
    assert "DT_NODELABEL(i2c22)" not in source
    assert "i2c_write_read(" in source
    assert "i2c_write(" in source
    assert "CONFIG_I2C=y" in config


def test_nrf_bringup_enters_ready_unassigned_runtime_state_without_forcing_role():
    source = MAIN.read_text(encoding="utf-8")

    assert '#include "nrf_node_state.h"' in source
    assert "nrf_node_state_init(&node_state);" in source
    assert "nrf_node_state_mark_ready(&node_state)" in source
    assert "nrf_node_state_assign_role(" not in source


def test_nrf_docs_record_the_same_verified_platform_revision():
    task = TEST_TASK.read_text(encoding="utf-8")
    readme = README.read_text(encoding="utf-8")

    assert VERIFIED_SEEED_PLATFORM in task
    assert "commit-pinned" in task
    assert "plain Seeed Studio XIAO nRF54L15" in readme
    assert re.search(r"not\s+the\s+XIAO\s+nRF54L15\s+Sense", readme)


def test_nrf_automatic_test_contract_does_not_equate_no_upload_with_native_execution():
    task = TEST_TASK.read_text(encoding="utf-8")

    assert "`pio test --without-uploading` prevents firmware" in task
    assert "embedded test can still require a physical test/serial port" in task
    assert "truly hardware-free native/host" in task
