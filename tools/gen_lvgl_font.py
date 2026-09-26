#!/usr/bin/env python3
"""Generate the LVGL 9 UI font from the pinned, vendored Montserrat TTF."""

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FONT = Path("components/ui/fonts/Montserrat-Regular.ttf")
FONT_SHA256 = "3e8abe50c44c82e2242e97d1ec8c0d385c4890cdc50447bcdb8605c81a38cfb2"
OUTPUT = Path("components/ui/fonts/ui_font_montserrat_14.c")
CONVERTER_VERSION = "1.5.3"
GLYPH_RANGES = (
    "0x20-0x7e,0xa0-0xff,0x400-0x4ff,"
    "0x2bc,0x2013-0x2014,0x2019,0x2212"
)


def main() -> int:
    font_path = ROOT / FONT
    digest = hashlib.sha256(font_path.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        print(f"unexpected Montserrat input SHA-256: {digest}", file=sys.stderr)
        return 1

    command = (
        f"npx --yes lv_font_conv@{CONVERTER_VERSION} "
        f"--font {FONT.as_posix()} -r {GLYPH_RANGES} "
        f"--size 14 --bpp 4 --format lvgl "
        f"--lv-font-name ui_font_montserrat_14 "
        f"--lv-fallback lv_font_montserrat_14 "
        f"--output {OUTPUT.as_posix()}"
    )
    subprocess.run(command, cwd=ROOT, shell=True, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
