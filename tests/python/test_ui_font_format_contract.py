from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
FONT_C = ROOT / "components/ui/fonts/ui_font_montserrat_14.c"
GENERATOR = ROOT / "tools/gen_lvgl_font.py"
DEFAULTS = ROOT / "sdkconfig.defaults"

# LVGL 9 lv_font_fmt_txt.h: 0 = plain bitmap, 1/2 = compressed (RLE).
LV_FONT_FMT_TXT_PLAIN = 0


def font_bitmap_format() -> int:
    text = FONT_C.read_text(encoding="utf-8")
    match = re.search(r"\.bitmap_format\s*=\s*(\d+)", text)
    assert match is not None, "generated font has no bitmap_format field"
    return int(match.group(1))


def test_generated_ui_font_is_decodable_with_the_build_configuration() -> None:
    """A compressed font renders as blank labels when the LVGL decoder is off.

    lv_font_fmt_txt.c returns a NULL glyph bitmap for compressed fonts unless
    CONFIG_LV_USE_FONT_COMPRESSED=y; widgets still draw, text does not. The
    generated sdkconfig is gitignored, so the safe contract is a plain font.
    """
    fmt = font_bitmap_format()
    defaults = DEFAULTS.read_text(encoding="utf-8")
    compressed_decoder_enabled = (
        "CONFIG_LV_USE_FONT_COMPRESSED=y" in defaults
    )
    assert fmt == LV_FONT_FMT_TXT_PLAIN or compressed_decoder_enabled, (
        "ui_font_montserrat_14.c is compressed but the LVGL compressed-font "
        "decoder is not enabled; every label would render empty"
    )


def test_font_generator_keeps_producing_a_plain_font() -> None:
    generator = GENERATOR.read_text(encoding="utf-8")
    assert "--no-compress" in generator
