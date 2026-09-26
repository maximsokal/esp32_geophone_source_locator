import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "i18n" / "tft.json"
FONT = ROOT / "components" / "ui" / "fonts" / "ui_font_montserrat_14.c"
FIRMWARE_CATALOG = ROOT / "components" / "ui_i18n" / "include" / "ui_i18n_catalog.gen.h"
PRINTF = re.compile(
    r"%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn])"
)


def printf_conversions(text):
    result = []
    offset = 0
    while offset < len(text):
        if text[offset] != "%":
            offset += 1
            continue
        match = PRINTF.match(text, offset)
        assert match is not None, f"invalid printf conversion in {text!r} at {offset}"
        result.append(match.group())
        offset = match.end()
    return result


def font_codepoints(source):
    unicode_lists = {}
    for match in re.finditer(
        r"static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};",
        source,
        re.DOTALL,
    ):
        unicode_lists[match.group(1)] = [
            int(value, 16)
            for value in re.findall(r"0x([0-9A-Fa-f]+)", match.group(2))
        ]

    cmap_text = source.split("static const lv_font_fmt_txt_cmap_t cmaps[]", 1)[1]
    cmap_text = cmap_text.split("};", 1)[0]
    result = set()
    maps = list(
        re.finditer(
            r"\.range_start\s*=\s*(\d+).*?\.range_length\s*=\s*(\d+)"
            r".*?\.unicode_list\s*=\s*(NULL|unicode_list_\d+)"
            r".*?\.list_length\s*=\s*(\d+)",
            cmap_text,
            re.DOTALL,
        )
    )
    assert maps, "generated LVGL font has no parseable cmap entries"
    for match in maps:
        start, length = int(match.group(1)), int(match.group(2))
        unicode_list, list_length = match.group(3), int(match.group(4))
        if unicode_list == "NULL":
            result.update(range(start, start + length))
        else:
            offsets = unicode_lists[unicode_list]
            assert len(offsets) == list_length
            result.update(start + value for value in offsets)
    return result


def test_tft_catalog_is_complete_and_lengths_cover_both_locales():
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    assert catalog["namespace"] == "tft"
    assert catalog["entries"]
    for key, entry in catalog["entries"].items():
        assert entry["en"], f"{key} has no English text"
        assert entry["uk"], f"{key} has no Ukrainian text"
        assert "'" not in entry["uk"], f"{key} must use U+2019 apostrophes"
        assert entry["max_len"] >= max(len(entry["en"]), len(entry["uk"])), key


def test_printf_conversion_sequences_match():
    entries = json.loads(CATALOG.read_text(encoding="utf-8"))["entries"]
    for key, entry in entries.items():
        assert printf_conversions(entry["en"]) == printf_conversions(entry["uk"]), key


def test_font_cmaps_cover_catalog_and_required_ukrainian_glyphs():
    entries = json.loads(CATALOG.read_text(encoding="utf-8"))["entries"]
    used = {
        ord(character)
        for entry in entries.values()
        for text in (entry["en"], entry["uk"])
        for character in text
    }
    glyphs = font_codepoints(FONT.read_text(encoding="utf-8"))
    required = set(range(0x0410, 0x0450))
    required.update(
        {
            0x0404, 0x0406, 0x0407, 0x0490,
            0x0454, 0x0456, 0x0457, 0x0491,
            0x00B0, 0x0025, 0x2212, 0x2013, 0x2014,
            0x2019, 0x02BC, 0x00B5,
        }
    )
    assert used <= glyphs, f"catalog glyphs missing from cmap: {sorted(used - glyphs)}"
    assert required <= glyphs, f"required glyphs missing from cmap: {sorted(required - glyphs)}"


def test_representative_longest_labels_match_max_len():
    entries = json.loads(CATALOG.read_text(encoding="utf-8"))["entries"]
    for key in ("system.language_footer", "wifi.notice_setup_open", "measurement.action_save"):
        entry = entries[key]
        assert entry["max_len"] == max(len(entry["en"]), len(entry["uk"])), key


def test_ui_translation_keys_exist_in_firmware_catalog():
    generated = FIRMWARE_CATALOG.read_text(encoding="utf-8")
    defined = set(re.findall(r"\bUI_I18N_KEY_[A-Z0-9_]+\b", generated))
    referenced = {
        match.group()
        for path in (ROOT / "components" / "ui").rglob("*")
        if path.suffix in {".c", ".h"}
        for match in re.finditer(
            r"\bUI_I18N_KEY_[A-Z0-9_]+\b",
            path.read_text(encoding="utf-8"),
        )
    }
    assert referenced <= defined
