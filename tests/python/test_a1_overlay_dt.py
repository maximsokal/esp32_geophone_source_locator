"""Verify PocketBeagle 2 A1 Device Tree Overlay (cpp + dtc + fdtoverlay + official A1 DTB)."""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OVL_DIR = REPO_ROOT / "PocketBeagle2" / "overlays" / "a1"
OVL_DTSO = OVL_DIR / "pb2-a1-ad7606-uart2.dtso"
BOARD_A1_JSON = REPO_ROOT / "PocketBeagle2" / "config" / "board_a1.json"

BEAGLE_DT_COMMIT = "7e56998024f3520b279ab482dbfa1bb355fa7f3f"
BEAGLE_DT_RAW = f"https://raw.githubusercontent.com/beagleboard/BeagleBoard-DeviceTrees/{BEAGLE_DT_COMMIT}"


def _to_wsl_path(win_path: Path) -> str:
    s = str(win_path.resolve()).replace("\\", "/")
    if len(s) >= 2 and s[1] == ":":
        return f"/mnt/{s[0].lower()}{s[2:]}"
    return s


def _compile_overlay_to_tmp(wsl_ovl_dir: str, wsl_ovl: str) -> None:
    cmd = (
        f"cpp -nostdinc -I '{wsl_ovl_dir}' -undef -x assembler-with-cpp '{wsl_ovl}' "
        f"/tmp/pb2_a1_ovl.dts.pp && "
        f"dtc -@ -I dts -O dtb -o /tmp/pb2_a1_ovl.dtbo /tmp/pb2_a1_ovl.dts.pp"
    )
    subprocess.run(["wsl", "bash", "-lc", cmd], check=True)


def _assert_merged_contract(merged: str) -> None:
    assert 'compatible = "beagle,am62-pocketbeagle2", "ti,am625"' in merged
    assert 'status = "okay";' in merged
    assert "pb2-a1-pru1-ad7606-pins" in merged
    assert "pb2-a1-uart2-pins" in merged
    assert 'compatible = "generic-uio";' in merged
    assert 'linux,uio-name = "pb2-pru-shared";' in merged
    assert "reg = <0x00 0x30050000 0x00 0x8000>;" in merged
    # Ensure &pru1 keeps remoteproc compatible and is not hijacked by UIO
    assert 'compatible = "ti,am625-pru";' in merged

    # Verify UART2_RXD uses PIN_INPUT_PULLUP (0x64004) and not floating 0x50004
    assert "0xb8 0x64004" in merged or "0x00b8 0x00064004" in merged
    assert "0x50004" not in merged

    # Verify every PAD offset from board_a1.json appears in the preprocessed overlay without duplicates
    pp = subprocess.run(
        ["wsl", "bash", "-lc", "cat /tmp/pb2_a1_ovl.dts.pp"],
        capture_output=True, text=True, check=True,
    ).stdout.lower()
    cfg = json.loads(BOARD_A1_JSON.read_text(encoding="utf-8"))
    expected_offsets = {
        cfg["uart2"]["pb2_rx"]["pad_offset"].lower(),
        cfg["uart2"]["pb2_tx"]["pad_offset"].lower(),
    }
    for name, info in cfg["ad7606"]["pru_pins"].items():
        if name != "gnd":
            expected_offsets.add(info["pad_offset"].lower())

    found_offsets = re.findall(r"\(\(\(0x[0-9a-f]+\)\s*&\s*0x1fff\)\)\s*\([^)]+\)", pp)
    assert len(found_offsets) == 10, f"Expected 10 AM62X_IOPAD entries, got {len(found_offsets)}"
    extracted = set(re.findall(r"\(\(\((0x[0-9a-f]+)\)\s*&\s*0x1fff\)\)", pp))
    assert extracted == expected_offsets


def test_a1_overlay_compiles_and_applies_to_synthetic_dtb() -> None:
    assert OVL_DTSO.exists(), f"Missing A1 overlay: {OVL_DTSO}"
    assert BOARD_A1_JSON.exists(), f"Missing A1 board config: {BOARD_A1_JSON}"
    assert shutil.which("wsl") is not None

    _compile_overlay_to_tmp(_to_wsl_path(OVL_DIR), _to_wsl_path(OVL_DTSO))

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
fdtoverlay -i /tmp/pb2_a1_base.dtb -o /tmp/pb2_a1_merged.dtb /tmp/pb2_a1_ovl.dtbo
dtc -I dtb -O dts /tmp/pb2_a1_merged.dtb
"""
    proc = subprocess.run(["wsl", "bash", "-lc", script], capture_output=True, text=True, check=True)
    _assert_merged_contract(proc.stdout)


def _ensure_dts_tree(cache_dir: Path, rel_path: str, seen: set[str]) -> None:
    if rel_path in seen:
        return
    seen.add(rel_path)
    dest = cache_dir / rel_path
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(f"{BEAGLE_DT_RAW}/{rel_path}", dest)
    text = dest.read_text(encoding="utf-8", errors="ignore")
    parent_rel = Path(rel_path).parent.as_posix()
    for inc in re.findall(r'#include\s+[<"]([^">]+)[">]', text):
        if inc.startswith("dt-bindings/"):
            _ensure_dts_tree(cache_dir, f"include/{inc}", seen)
        else:
            child_rel = f"{parent_rel}/{inc}" if parent_rel != "." else inc
            _ensure_dts_tree(cache_dir, child_rel, seen)


def test_a1_overlay_applies_to_official_pocketbeagle2_dtb() -> None:
    cache_dir = REPO_ROOT / ".test-build" / "beagle_dt_cache" / BEAGLE_DT_COMMIT
    inc_dir = cache_dir / "include"
    dts_dir = cache_dir / "src" / "arm64" / "ti"
    _ensure_dts_tree(cache_dir, "src/arm64/ti/k3-am62-pocketbeagle2.dts", set())

    _compile_overlay_to_tmp(_to_wsl_path(OVL_DIR), _to_wsl_path(OVL_DTSO))
    wsl_dts = _to_wsl_path(dts_dir / "k3-am62-pocketbeagle2.dts")
    wsl_dts_dir = _to_wsl_path(dts_dir)
    wsl_inc_dir = _to_wsl_path(inc_dir)

    script = f"""
set -e
cpp -nostdinc -I '{wsl_inc_dir}' -I '{wsl_dts_dir}' -undef -x assembler-with-cpp '{wsl_dts}' /tmp/pb2_official_a1.dts.pp
dtc -@ -I dts -O dtb -o /tmp/pb2_official_a1.dtb /tmp/pb2_official_a1.dts.pp
fdtoverlay -i /tmp/pb2_official_a1.dtb -o /tmp/pb2_official_a1_merged.dtb /tmp/pb2_a1_ovl.dtbo
dtc -I dtb -O dts /tmp/pb2_official_a1_merged.dtb
"""
    proc = subprocess.run(["wsl", "bash", "-lc", script], capture_output=True, text=True, check=True)
    merged = proc.stdout
    _assert_merged_contract(merged)
    # Verify official AM625 PRUSS power-domains & shrdram2 remain intact alongside generic-uio
    assert "power-domains" in merged
    assert "shrdram2" in merged
