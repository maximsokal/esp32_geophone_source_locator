import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1].parent
MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "max483b-periodic.json"


def _configured_tick_rate(path: Path) -> int:
    matches = re.findall(r"^CONFIG_FREERTOS_HZ=(\d+)$", path.read_text(encoding="utf-8"), re.MULTILINE)
    assert len(matches) == 1, f"expected one CONFIG_FREERTOS_HZ in {path.name}"
    return int(matches[0])


def test_host_tick_contract_matches_both_esp_idf_configs():
    tick_rate = _configured_tick_rate(ROOT / "sdkconfig")
    assert tick_rate == _configured_tick_rate(ROOT / "sdkconfig.defaults")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    tick_test = next(test for test in manifest["tests"] if test["name"] == "max483b_tick_periodic")
    workflow_test = next(test for test in manifest["tests"] if test["name"] == "max483b_workflow_periodic")
    assert f"CONFIG_FREERTOS_HZ={tick_rate}" in tick_test["defines"]
    assert f"CONFIG_FREERTOS_HZ={tick_rate}" in workflow_test["defines"]
