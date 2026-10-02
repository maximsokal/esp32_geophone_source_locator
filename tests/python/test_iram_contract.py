from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_iram_contract",
    ROOT / "tools" / "check_iram_contract.py",
)
assert SPEC is not None and SPEC.loader is not None
iram_contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(iram_contract)


NM_FIXTURE = """\
40370010 00000020 t ad7606_busy_isr
3fc00000 00000001 b s_running
3fc00004 00000004 b s_task
"""

MAP_FIXTURE = """\
.iram0.text 0x40370000 0x00000040
.iram0.text_end
  0x40370040 0x00000010
.dram0.bss 0x3fc00000 0x00000040
 .bss.s_running
   0x3fc00000 0x00000001 esp-idf/ad7606_acquisition/libad7606_acquisition.a(ad7606_acquisition.c.obj)
 .bss.s_task 0x3fc00004 0x00000004 esp-idf/ad7606_acquisition/libad7606_acquisition.a(ad7606_acquisition.c.obj)
.flash.text 0x42000000 0x00000100
.ext_ram.bss 0x3c000000 0x00000100
"""


def test_fixture_in_iram_and_internal_dram_passes() -> None:
    result = iram_contract.audit_texts(NM_FIXTURE, MAP_FIXTURE, 0x50)
    assert "ad7606_busy_isr 0x40370010 .iram0.text" in result
    assert "s_running 0x3fc00000 .dram0.bss" in result
    assert "s_task 0x3fc00004 .dram0.bss" in result
    assert result[-1] == "IRAM 80 bytes (0x50) / budget 80 bytes (0x50)"


def test_isr_moved_to_flash_fails() -> None:
    nm_text = NM_FIXTURE.replace("40370010 00000020 t ad7606_busy_isr",
                                 "42000010 00000020 t ad7606_busy_isr")
    with pytest.raises(iram_contract.ContractError, match=r"ad7606_busy_isr.*\.flash\.text"):
        iram_contract.audit_texts(nm_text, MAP_FIXTURE)


def test_isr_data_in_external_ram_fails() -> None:
    nm_text = NM_FIXTURE.replace("3fc00000 00000001 b s_running",
                                 "3c000020 00000001 b s_running")
    map_text = MAP_FIXTURE.replace("0x3fc00000 0x00000001 esp-idf/",
                                   "0x3c000020 0x00000001 esp-idf/")
    with pytest.raises(iram_contract.ContractError, match=r"s_running.*\.ext_ram\.bss"):
        iram_contract.audit_texts(nm_text, map_text)


def test_iram_budget_exceeded_fails() -> None:
    with pytest.raises(iram_contract.ContractError, match=r"allocated IRAM is 80 bytes"):
        iram_contract.audit_texts(NM_FIXTURE, MAP_FIXTURE, 0x4F)


def test_real_default_build_artifacts_when_present() -> None:
    elf = ROOT / "build" / "esp32_geophone_source_locator.elf"
    map_file = ROOT / "build" / "esp32_geophone_source_locator.map"
    if not elf.is_file() or not map_file.is_file():
        pytest.skip("default firmware ELF/map artifacts are absent")
    result = iram_contract.check_artifacts(elf, map_file)
    assert any(line.startswith("ad7606_busy_isr ") for line in result)
