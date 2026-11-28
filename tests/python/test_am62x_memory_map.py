"""Verify AM62x PRU-SS shared memory map consistency across header, linker, DTS/sim, and C++ runtime."""
from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_H = REPO_ROOT / "PocketBeagle2" / "pru" / "include" / "pru_adc_shared.h"
LINKER_CMD = REPO_ROOT / "PocketBeagle2" / "pru" / "linker" / "AM62x_PRU1.cmd"
SIM_CFG = REPO_ROOT / ".tools" / "pru-simulator" / "config" / "memory_am62x.cfg"
MAIN_CPP = REPO_ROOT / "PocketBeagle2" / "src" / "main.cpp"
PIPELINE_HPP = REPO_ROOT / "PocketBeagle2" / "include" / "pb2" / "runtime" / "pipeline.hpp"


def test_am62x_pruss_memory_map_matches_official_contract() -> None:
    hdr = SHARED_H.read_text(encoding="utf-8")
    assert re.search(r"#define\s+PB2_PRU_SHARED_MEM_ADDR\s+0x00010000U", hdr)
    assert re.search(r"#define\s+PB2_PRU_SHARED_MEM_SIZE\s+0x00008000U", hdr)
    assert re.search(r"#define\s+PB2_AM62X_PRUSS_BASE_ADDR\s+0x30040000UL", hdr)
    assert re.search(r"#define\s+PB2_AM62X_PRU_SHARED_PHYS\s+0x30050000UL", hdr)

    cmd = LINKER_CMD.read_text(encoding="utf-8")
    assert re.search(r"PRU_SHAREDMEM\s*:\s*org\s*=\s*0x00010000\s+len\s*=\s*0x00008000", cmd)
    assert re.search(r"\.pb2_shared\s*:\s*>\s*PRU_SHAREDMEM,\s*PAGE\s*1", cmd)

    if SIM_CFG.exists():
        sim_txt = SIM_CFG.read_text(encoding="utf-8")
        assert re.search(r"\[ICSS_SHARED\][^\[]*base\s*=\s*0x00010000[^\[]*size\s*=\s*0x8000", sim_txt, re.S)

    for cpp in (MAIN_CPP, PIPELINE_HPP):
        txt = cpp.read_text(encoding="utf-8")
        assert "0x30010000" not in txt, f"Stale erroneous address 0x30010000 found in {cpp}"
        assert "PB2_AM62X_PRU_SHARED_PHYS" in txt
