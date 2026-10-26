"""Guards for docs/peripheral-signals.json (Wi-Fi, HTTP, mux, SD, Channel Sounding)."""
import copy
import json
from pathlib import Path
import re
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import run_peripheral_signal_checks as checks  # noqa: E402
from run_host_tests import RunnerError  # noqa: E402


@pytest.fixture(scope="module")
def data():
    return checks.load_map(ROOT)


def test_map_matches_the_tree(data):
    entries = checks.check_map(ROOT, data)
    assert len(entries) == len(data["suites"])
    assert {row["family"] for row in data["contracts"]} == checks.FAMILIES


def test_every_contract_finding_and_observation_is_documented(data):
    text = (ROOT / "docs/peripheral-signals.md").read_text(encoding="utf-8")
    for row in data["contracts"]:
        assert f"`{row['id']}`" in text, row["id"]
    for item in data["findings"] + data["observations"]:
        assert f"**{item['id']}**" in text, item["id"]
    for row in data["capabilities"]["CS"]["rows"]:
        assert row["signal"] in text, row["signal"]


def _must_fail(data, mutate, message):
    broken = copy.deepcopy(data)
    mutate(broken)
    with pytest.raises(RunnerError, match=message or None):
        checks.check_map(ROOT, broken)


def test_negative_controls_reject_a_corrupted_map(data):
    first = data["contracts"][0]
    _must_fail(data, lambda d: d["contracts"][0].update(tests=["no_such_suite"]), "unknown suite")
    _must_fail(data, lambda d: d["contracts"][0].update(sources=["components/nope.c"]), "")
    _must_fail(data, lambda d: d["contracts"].append(copy.deepcopy(first)), "duplicate contract id")
    _must_fail(data, lambda d: d["contracts"][0].update(family="OTHER"), "bad family")
    _must_fail(data, lambda d: d["contracts"][0].pop("reaction"), "missing reaction")
    _must_fail(data, lambda d: d["suites"].update(extra_suite={"manifest": "tests/host/manifest.json"}), "not used by any contract")
    _must_fail(data, lambda d: d["suites"]["peripheral_signal_mux"]["cases"].append("MUX.invented"), "declared cases differ")
    _must_fail(data, lambda d: d["suites"]["peripheral_signal_mux"]["cases"].pop(), "declared cases differ")
    _must_fail(data, lambda d: d["findings"][0].update(case="CS.ADAPTER.invented"), "not a declared signal case")
    _must_fail(data, lambda d: d["findings"][0].update(files=["nope.c"]), "")
    _must_fail(data, lambda d: d["declarations"][0]["members"].pop(), "changed")
    _must_fail(data, lambda d: d["capabilities"]["CS"]["rows"][0].update(status="made-up"), "bad status")
    _must_fail(data, lambda d: d["suites"]["tca9548a_mux"].update(manifest="tests/host/manifest.d/coverage-core.json"),
               "missing/non-unique suite")


def test_every_signal_suite_case_is_printed_by_its_test_source(data):
    for name, spec in data["suites"].items():
        if not name.startswith("peripheral_signal_"):
            continue
        manifest = json.loads((ROOT / spec["manifest"]).read_text(encoding="utf-8"))["tests"]
        entry = next(item for item in manifest if item["name"] == name)
        source = next(item for item in entry["sources"] if item.endswith("_test.c"))
        text = (ROOT / source).read_text(encoding="utf-8")
        assert set(re.findall(r'CASE_PASS\("([^"]+)"\)', text)) == set(spec["cases"]), name
        assert "setvbuf(stdout" in text, f"{name}: progress must survive a crash"


def test_wifi_setup_body_limit_covers_the_largest_legal_credentials():
    # 32-byte SSID + 63-byte password fully percent-encoded + "ssid=" + "&password=" + terminator.
    required = 3 * (32 + 63) + len("ssid=") + len("&password=") + 1
    internal = (ROOT / "components/web_server/web_server_internal.h").read_text(encoding="utf-8")
    stub = (ROOT / "tests/host/stubs/web_wifi_input/web_server_internal.h").read_text(encoding="utf-8")
    limits = [int(re.search(r"#define WEB_WIFI_SETUP_BODY_MAX (\d+)U", text)[1]) for text in (internal, stub)]
    assert limits[0] == limits[1] >= required
    handler = (ROOT / "components/web_server/web_wifi_setup.c").read_text(encoding="utf-8")
    assert re.search(r"#define WIFI_SETUP_BODY_SIZE\s+WEB_WIFI_SETUP_BODY_MAX", handler)
    form = (ROOT / "components/web_server/web_wifi_setup_form.c").read_text(encoding="utf-8")
    assert "char copy[WEB_WIFI_SETUP_BODY_MAX]" in form


def test_new_signal_fragment_is_in_the_standard_host_gate():
    fragment = ROOT / "tests/host/manifest.d/coverage-peripheral-signals.json"
    names = [item["name"] for item in json.loads(fragment.read_text(encoding="utf-8"))["tests"]]
    assert len(names) == len(set(names)) == 9
    assert all(name.startswith("peripheral_signal_") for name in names)
