#!/usr/bin/env python3
"""Check the default ESP32-S3 BUSY ISR placement and allocated IRAM budget."""

# MAX-368 Phase 1 audit, build/esp32_geophone_source_locator.elf + .map:
# ad7606_busy_isr 0x40377250 is in .iram0.text; it calls
# vTaskGenericNotifyGiveFromISR 0x403828b4 and, on wake, _frxt_setup_switch
# 0x4037f918, also in .iram0.text. board_gpio_isr_service_install installs
# the service with ESP_INTR_FLAG_IRAM. Transitive .iram0.text callees include
# xPortEnterCriticalTimeout 0x4037f6cc, vPortExitCritical 0x4037f81c,
# prvCheckTaskCanBeScheduledSMP 0x40380628, prvIsYieldRequiredSMP 0x403806ac,
# esp_cpu_compare_and_set 0x4037bd80, vPortYieldOtherCore 0x4037f8b8,
# __assert_func 0x4038844c, spi_flash_cache_enabled 0x403782bc, and
# esp_system_abort 0x4037e75c. ROM callees include memset 0x400011e8, memcpy
# 0x400011f4, strlen 0x40001248, itoa 0x400014c4, and _xtos_set_intlevel
# 0x40001c38. The ISR's s_running 0x3fcad170 and s_task
# 0x3fcad174 are .dram0.bss; FreeRTOS scheduler/critical state referenced by
# the helper is also .dram0.data/.dram0.bss. The source/disassembly has no
# logging, allocation, floating math, SPI transaction, or task-blocking
# operation in the ISR; spi_flash_cache_enabled is only on an IDF assert path.
# Allocated .iram0.* output sections total 0x1b000 (108 KiB); the reviewed
# budget is 0x1c000 (112 KiB), measured usage plus 4 KiB headroom for small
# build growth. Verdict: profiled/audited - no additional IRAM placement
# justified.

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
IRAM_BASELINE_BYTES = 0x1B000
IRAM_REVIEWED_MARGIN_BYTES = 0x1000
IRAM_BUDGET_BYTES = IRAM_BASELINE_BYTES + IRAM_REVIEWED_MARGIN_BYTES
ISR_SYMBOL = "ad7606_busy_isr"
ISR_DATA_SYMBOLS = ("s_running", "s_task")

_NM_ROW = re.compile(
    r"^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z?])\s+(\S+)\s*$"
)
_MAP_SECTION = re.compile(
    r"^(\.[^\s]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s|$)"
)
_MAP_SECTION_HEAD = re.compile(r"^(\.[^\s]+)\s*$")
_MAP_SECTION_CONT = re.compile(
    r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s|$)"
)
_MAP_ISR_DATA_INLINE = re.compile(
    r"^\s+\.(?:bss|data)\.(s_running|s_task)\s+"
    r"0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+?)\s*$"
)
_MAP_ISR_DATA_HEAD = re.compile(r"^\s+\.(?:bss|data)\.(s_running|s_task)\s*$")
_MAP_ISR_DATA_CONT = re.compile(
    r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+?)\s*$"
)


class ContractError(ValueError):
    pass


def parse_nm(text: str) -> list[tuple[int, int, str, str]]:
    symbols = []
    for line in text.splitlines():
        match = _NM_ROW.match(line)
        if match:
            address, size, symbol_type, name = match.groups()
            symbols.append((int(address, 16), int(size, 16), symbol_type, name))
    return symbols


def parse_map_sections(text: str) -> list[tuple[str, int, int]]:
    sections = []
    pending_name = None
    for line in text.splitlines():
        match = _MAP_SECTION.match(line)
        if match:
            name, address, size = match.groups()
            if "dummy" not in name.lower() and int(size, 16) != 0:
                sections.append((name, int(address, 16), int(size, 16)))
            pending_name = None
            continue
        if pending_name is not None:
            match = _MAP_SECTION_CONT.match(line)
            if match:
                address, size = match.groups()
                if "dummy" not in pending_name.lower() and int(size, 16) != 0:
                    sections.append((pending_name, int(address, 16), int(size, 16)))
            pending_name = None
            continue
        match = _MAP_SECTION_HEAD.match(line)
        if match:
            pending_name = match.group(1)
    return sections


def parse_isr_data_symbols(text: str) -> list[tuple[str, int, int]]:
    symbols = []
    pending_name = None
    for line in text.splitlines():
        match = _MAP_ISR_DATA_INLINE.match(line)
        if match:
            name, address, size, owner = match.groups()
            if "ad7606_acquisition.c.obj" in owner:
                symbols.append((name, int(address, 16), int(size, 16)))
            pending_name = None
            continue
        if pending_name is not None:
            match = _MAP_ISR_DATA_CONT.match(line)
            if match:
                address, size, owner = match.groups()
                if "ad7606_acquisition.c.obj" in owner:
                    symbols.append((pending_name, int(address, 16), int(size, 16)))
            pending_name = None
            continue
        match = _MAP_ISR_DATA_HEAD.match(line)
        if match:
            pending_name = match.group(1)
    return symbols


def _section_for(address: int, sections: list[tuple[str, int, int]]) -> str | None:
    matches = [
        section for section in sections
        if section[1] <= address < section[1] + section[2]
    ]
    if not matches:
        return None
    return min(matches, key=lambda section: section[2])[0]


def audit_texts(
    nm_text: str,
    map_text: str,
    iram_budget_bytes: int = IRAM_BUDGET_BYTES,
) -> list[str]:
    symbols = parse_nm(nm_text)
    sections = parse_map_sections(map_text)
    data_symbols = parse_isr_data_symbols(map_text)
    if not sections:
        raise ContractError("linker map has no sized output-section records")

    errors = []
    report = []
    isr = [symbol for symbol in symbols if symbol[3] == ISR_SYMBOL]
    if len(isr) != 1:
        errors.append(f"expected one defined {ISR_SYMBOL}, found {len(isr)}")
    else:
        address, _, _, name = isr[0]
        section = _section_for(address, sections)
        if section is None or not section.startswith(".iram0."):
            errors.append(
                f"{name} at 0x{address:08x} is in {section or 'an unmapped section'}, "
                "expected .iram0.*"
            )
        else:
            report.append(f"{name} 0x{address:08x} {section}")

    for name in ISR_DATA_SYMBOLS:
        entries = [symbol for symbol in data_symbols if symbol[0] == name]
        if not entries:
            errors.append(f"approved ISR data symbol {name} is missing from its object map")
            continue
        for _, address, _ in entries:
            nm_entries = [
                symbol for symbol in symbols
                if symbol[0] == address and symbol[3] == name
            ]
            if not nm_entries:
                errors.append(
                    f"{name} at 0x{address:08x} from the component map is missing in nm"
                )
                continue
            section = _section_for(address, sections)
            for _, _, symbol_type, _ in nm_entries:
                if symbol_type.lower() not in ("b", "d"):
                    errors.append(
                        f"{name} at 0x{address:08x} has non-mutable nm type "
                        f"{symbol_type!r}"
                    )
            if section is None or not section.startswith((".dram0.", ".iram0.")):
                errors.append(
                    f"{name} at 0x{address:08x} is in "
                    f"{section or 'an unmapped section'}, expected internal DRAM/IRAM"
                )
            else:
                report.append(f"{name} 0x{address:08x} {section}")

    iram_bytes = sum(
        size for name, _, size in sections if name.startswith(".iram0.")
    )
    if iram_bytes == 0:
        errors.append("linker map has no allocated .iram0.* sections")
    if iram_bytes > iram_budget_bytes:
        errors.append(
            f"allocated IRAM is {iram_bytes} bytes (0x{iram_bytes:x}), over "
            f"budget {iram_budget_bytes} bytes (0x{iram_budget_bytes:x})"
        )
    else:
        report.append(
            f"IRAM {iram_bytes} bytes (0x{iram_bytes:x}) / "
            f"budget {iram_budget_bytes} bytes (0x{iram_budget_bytes:x})"
        )

    if errors:
        raise ContractError("; ".join(errors))
    return report


def _find_nm(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if path.is_file():
            return path
        raise ContractError(f"nm executable not found: {path}")

    tool_name = "xtensa-esp32s3-elf-nm.exe" if os.name == "nt" else "xtensa-esp32s3-elf-nm"
    roots = [Path(r"C:\Espressif\tools\xtensa-esp-elf")]
    idf_tools_path = os.environ.get("IDF_TOOLS_PATH")
    if idf_tools_path:
        roots.append(Path(idf_tools_path) / "tools" / "xtensa-esp-elf")
    for root in roots:
        for version_dir in sorted(root.glob("*"), reverse=True):
            candidate = version_dir / "xtensa-esp-elf" / "bin" / tool_name
            if candidate.is_file():
                return candidate
    on_path = shutil.which("xtensa-esp32s3-elf-nm")
    if on_path:
        return Path(on_path)
    raise ContractError(
        "xtensa-esp32s3-elf-nm not found; add it to PATH, set IDF_TOOLS_PATH, "
        "or pass --nm"
    )


def check_artifacts(
    elf_path: Path,
    map_path: Path,
    nm_path: str | None = None,
) -> list[str]:
    if not elf_path.is_file():
        raise ContractError(f"ELF artifact not found: {elf_path}")
    if not map_path.is_file():
        raise ContractError(f"linker map not found: {map_path}")
    nm = _find_nm(nm_path)
    result = subprocess.run(
        [str(nm), "-an", "-S", str(elf_path)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode:
        raise ContractError(f"nm failed ({result.returncode}): {result.stderr.strip()}")
    return audit_texts(result.stdout, map_path.read_text(encoding="utf-8", errors="replace"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=ROOT / "build/esp32_geophone_source_locator.elf")
    parser.add_argument("--map", type=Path, default=ROOT / "build/esp32_geophone_source_locator.map")
    parser.add_argument("--nm", help="xtensa-esp32s3-elf-nm executable (default: PATH/IDF tools)")
    args = parser.parse_args(argv)
    try:
        for line in check_artifacts(args.elf, args.map, args.nm):
            print(f"PASS {line}")
    except (ContractError, OSError) as error:
        print(f"IRAM contract failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
