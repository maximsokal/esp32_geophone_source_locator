"""Every SD access of the scan store runs under the storage operation (the SD's SPI arbitration).

Board 2026-11-17 (15-59-27 log): a phone asked for the scan's info, web_scan_recover_file() called stat() three times from the httpd
task with no storage operation held while the recorder was on the same SPI device, and the driver asserted
(spi_hal_setup_trans: spi_ll_get_running_cmd(hw) == 0) -- the board rebooted on every such request, so the 3D scan could never be
saved or shown. The guard is recursive, so holding it twice is harmless.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "components/web_server/web_scan_store.c").read_text(encoding="utf-8", errors="replace")

FUNCTION = re.compile(r"\n((?:static )?[A-Za-z_][\w \*]*?\b(web_scan_\w+)\s*\([^;{]*?\))\s*\n\{")


def _functions() -> dict[str, str]:
    found = [(m.start(), m.group(2)) for m in FUNCTION.finditer(SOURCE)]
    found.append((len(SOURCE), "END"))
    return {name: SOURCE[start:found[i + 1][0]] for i, (start, name) in enumerate(found[:-1])}


def test_recover_file_takes_the_storage_operation_before_it_touches_the_card() -> None:
    body = _functions()["web_scan_recover_file"]
    begin = body.index("storage_manager_operation_begin()")
    assert begin < body.index("web_scan_file_exists(")
    # ...and releases it before the recovery chunk, which takes its own admission and storage lock.
    assert body.index("storage_manager_operation_end()") < body.index("storage_bulk_admission_run_chunk(")


def test_every_user_of_stat_holds_the_storage_operation_or_says_the_caller_does() -> None:
    offenders = []
    for name, body in _functions().items():
        if name == "web_scan_file_exists" or "web_scan_file_exists(" not in body:
            continue
        if "storage_manager_operation_begin()" in body or name.endswith("_locked"):
            continue
        offenders.append(name)
    assert offenders == []
