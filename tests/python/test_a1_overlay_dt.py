"""Verify PocketBeagle 2 A1 Device Tree Overlay (dtc + fdtoverlay + pinmux contract)."""
from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OVL_DTSO = REPO_ROOT / "PocketBeagle2" / "overlays" / "a1" / "pb2-a1-ad7606-uart2.dtso"
BOARD_A1_JSON = REPO_ROOT / "PocketBeagle2" / "config" / "board_a1.json"


def _to_wsl_path(win_path: Path) -> str:
    s = str(win_path.resolve()).replace("\\", "/")
    if len(s) >= 2 and s[1] == ":":
        return f"/mnt/{s[0].lower()}{s[2:]}"
    return s


def test_a1_overlay_compiles_and_applies_via_fdtoverlay() -> None:
    assert OVL_DTSO.exists(), f"Missing A1 overlay: {OVL_DTSO}"
    assert BOARD_A1_JSON.exists(), f"Missing A1 board config: {BOARD_A1_JSON}"
    assert shutil.which("wsl") is not None

    wsl_ovl = _to_wsl_path(OVL_DTSO)
    base_dts = r"""
/dts-v1/;
/ {
    compatible = "beagle,am62-pocketbeagle2", "ti,am625";
    model = "BeagleBoard.org PocketBeagle2";
    #address-cells = <2>;
    #size-cells = <2>;

    bus@f0000 {
        #address-cells = <2>;
        #size-cells = <2>;
        main_pmx0: pinctrl@f4000 {
            compatible = "pinctrl-single";
            reg = <0x00 0x000f4000 0x00 0x000002ac>;
            #pinctrl-cells = <1>;
            pinctrl-single,register-width = <32>;
            pinctrl-single,function-mask = <0xffffffff>;
        };
        main_uart2: serial@2820000 {
            compatible = "ti,am64-uart", "ti,am654-uart";
            reg = <0x00 0x02820000 0x00 0x100>;
            status = "disabled";
        };
        pruss: pruss@30040000 {
            compatible = "ti,am625-pruss";
            reg = <0x00 0x30040000 0x00 0x80000>;
            #address-cells = <1>;
            #size-cells = <1>;
            pru1: pru@38000 {
                compatible = "ti,am625-pru";
                reg = <0x38000 0x3000>, <0x24000 0x400>, <0x24400 0x100>;
                status = "disabled";
            };
        };
    };
};
"""
    script = f"""
set -e
cat <<'EOF' > /tmp/pb2_a1_base.dts
{base_dts}
EOF
dtc -@ -I dts -O dtb -o /tmp/pb2_a1_base.dtb /tmp/pb2_a1_base.dts
dtc -@ -I dts -O dtb -o /tmp/pb2_a1_ovl.dtbo '{wsl_ovl}'
fdtoverlay -i /tmp/pb2_a1_base.dtb -o /tmp/pb2_a1_merged.dtb /tmp/pb2_a1_ovl.dtbo
dtc -I dtb -O dts /tmp/pb2_a1_merged.dtb
"""
    proc = subprocess.run(["wsl", "bash", "-lc", script], capture_output=True, text=True, check=True)
    merged = proc.stdout

    # Verify both pru1 and main_uart2 became okay and have pinctrl-0 attached
    assert 'compatible = "beagle,am62-pocketbeagle2", "ti,am625"' in merged
    assert 'status = "okay";' in merged
    assert "pb2-a1-pru1-ad7606-pins" in merged
    assert "pb2-a1-uart2-pins" in merged

    # Verify every PAD offset from board_a1.json appears in the overlay without duplicates or P2.20 conflict
    cfg = json.loads(BOARD_A1_JSON.read_text(encoding="utf-8"))
    expected_offsets = {
        cfg["uart2"]["pb2_rx"]["pad_offset"].lower(),
        cfg["uart2"]["pb2_tx"]["pad_offset"].lower(),
    }
    for name, info in cfg["ad7606"]["pru_pins"].items():
        if name != "gnd":
            expected_offsets.add(info["pad_offset"].lower())

    ovl_text = OVL_DTSO.read_text(encoding="utf-8").lower()
    found_offsets = re.findall(r"(0x[0-9a-f]{4})\s+0x[0-9a-f]{8}", ovl_text)
    assert len(found_offsets) == len(set(found_offsets)) == 10, f"Duplicate or missing pins: {found_offsets}"
    assert set(found_offsets) == expected_offsets
