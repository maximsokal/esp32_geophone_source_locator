from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / "nRF54L15" / "zephyr" / "app.overlay"
BUILT_DTS = (Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".ncs-workspaces" / "esp32_geophone_source_locator"
             / "nrf54l15-production-v3.4.0" / "zephyr" / "zephyr" / "zephyr.dts")

# CS-XIAO-RFSW-01. The XIAO nRF54L15 powers its antenna switch from P2.03 and selects the antenna with P2.05 (Seeed wiki, schematic;
# a forum user lost ten dB of RSSI at 5 m until the power pin was driven). P2.05 LOW is the on-board ceramic antenna, P2.03 HIGH
# powers the switch. The owner connects an EXTERNAL antenna to every XIAO (2026-11-03), so P2.05 is driven HIGH. The overlay says so with
# regulator-fixed nodes (enable-gpios ACTIVE_HIGH + regulator-boot-on = the pin is driven high at boot); this pins what the firmware and the
# built tree really contain, so a change of board definition cannot silently move the radio back to the ceramic antenna.
GPIO_ACTIVE_LOW = 1
GPIO_ACTIVE_HIGH = 0


def _overlay_node(text: str, label: str) -> str:
    match = re.search(r"&" + label + r"\s*\{(.*?)\};", text, re.S)
    assert match, f"{label} is not set in app.overlay"
    return match.group(1)


def test_the_overlay_powers_the_switch_and_selects_the_external_antenna():
    text = OVERLAY.read_text(encoding="utf-8")
    power = _overlay_node(text, "rfsw_pwr")
    select = _overlay_node(text, "rfsw_ctl")
    assert re.search(r"enable-gpios\s*=\s*<&gpio2\s+3\s+GPIO_ACTIVE_HIGH>", power), "P2.03 must power the RF switch (active high)"
    assert re.search(r"enable-gpios\s*=\s*<&gpio2\s+5\s+GPIO_ACTIVE_HIGH>", select), "P2.05 must be driven high: external antenna"
    assert "regulator-boot-on" in power and "regulator-boot-on" in select     # applied at boot, nothing has to toggle them later


@pytest.mark.skipif(
    not BUILT_DTS.exists() or BUILT_DTS.stat().st_mtime < OVERLAY.stat().st_mtime,
    reason="the production nRF build is missing or older than app.overlay (it is checked after the next build)",
)
def test_the_built_device_tree_has_the_same_pins_and_levels():
    text = BUILT_DTS.read_text(encoding="utf-8", errors="replace")

    def node(name: str) -> str:
        match = re.search(name + r":\s*[\w-]+\s*\{(.*?)\n\t\};", text, re.S)
        assert match, f"{name} is missing from the built device tree"
        return match.group(1)

    power = node("rfsw_pwr")
    select = node("rfsw_ctl")
    assert re.search(r"enable-gpios\s*=\s*<\s*&gpio2\s+0x3\s+0x%x\s*>" % GPIO_ACTIVE_HIGH, power)
    assert re.search(r"enable-gpios\s*=\s*<\s*&gpio2\s+0x5\s+0x%x\s*>" % GPIO_ACTIVE_HIGH, select)
    assert "regulator-boot-on" in power and "regulator-boot-on" in select
    assert 'compatible = "regulator-fixed"' in power and 'compatible = "regulator-fixed"' in select
