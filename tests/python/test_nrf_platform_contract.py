from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO = REPO_ROOT / "nRF54L15" / "platformio.ini"
README = REPO_ROOT / "nRF54L15" / "README.md"
TEST_TASK = REPO_ROOT / "docs" / "nrf54l15-test-task.md"
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
