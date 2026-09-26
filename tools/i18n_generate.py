#!/usr/bin/env python3
"""Validate i18n JSON catalogs and generate the WebUI and firmware catalogs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
NAMESPACES = ("common", "web", "report", "tft")
KEY_PATTERN = re.compile(r"^[a-z0-9]+(?:[._][a-z0-9]+)*$")
PLACEHOLDER_PATTERN = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
ENTRY_FIELDS = {"en", "uk", "note", "max_len"}


class CatalogError(ValueError):
    """A source catalog is invalid."""


def load_catalogs(root: Path) -> tuple[dict[str, dict[str, Any]], int]:
    entries: dict[str, dict[str, Any]] = {}
    untranslated = 0

    for namespace in NAMESPACES:
        path = root / "i18n" / f"{namespace}.json"
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise CatalogError(f"{path}: cannot read catalog: {exc}") from exc
        if not isinstance(raw, dict):
            raise CatalogError(f"{path}: catalog must be an object")
        unknown_catalog_fields = set(raw) - {"schema", "namespace", "entries"}
        if unknown_catalog_fields:
            fields = ", ".join(sorted(unknown_catalog_fields))
            raise CatalogError(f"{path}: unknown catalog field(s): {fields}")
        if type(raw.get("schema")) is not int or raw["schema"] != 1:
            raise CatalogError(f"{path}: schema must be 1")
        if raw.get("namespace") != namespace:
            raise CatalogError(f"{path}: namespace must be {namespace!r}")
        source_entries = raw.get("entries")
        if not isinstance(source_entries, dict):
            raise CatalogError(f"{path}: entries must be an object")

        for key, value in source_entries.items():
            location = f"{path}: entry {key!r}"
            if not isinstance(key, str) or not KEY_PATTERN.fullmatch(key):
                raise CatalogError(f"{location}: key must use lowercase letters, digits, dots, and underscores")
            if key in entries:
                raise CatalogError(f"{location}: duplicate key across catalogs")
            if not isinstance(value, dict):
                raise CatalogError(f"{location}: entry must be an object")

            unknown_entry_fields = set(value) - ENTRY_FIELDS
            if unknown_entry_fields:
                fields = ", ".join(sorted(unknown_entry_fields))
                raise CatalogError(f"{location}: unknown locale or entry field(s): {fields}")
            if "en" not in value or "uk" not in value:
                raise CatalogError(f"{location}: both en and uk fields are required")

            english = value["en"]
            ukrainian = value["uk"]
            for locale, text in (("en", english), ("uk", ukrainian)):
                if not isinstance(text, str):
                    raise CatalogError(f"{location}: {locale} must be a string")
                if text != "" and not text.strip():
                    raise CatalogError(f"{location}: {locale} must not be whitespace-only")
                if locale == "en" and text == "":
                    raise CatalogError(f"{location}: en must not be empty")

            if "note" in value and not isinstance(value["note"], str):
                raise CatalogError(f"{location}: note must be a string")

            if "max_len" in value:
                max_len = value["max_len"]
                if namespace != "tft":
                    raise CatalogError(f"{location}: max_len is allowed only in the tft catalog")
                if type(max_len) is not int or max_len < 1:
                    raise CatalogError(f"{location}: max_len must be a positive integer")
                for locale, text in (("en", english), ("uk", ukrainian)):
                    if text and len(text) > max_len:
                        raise CatalogError(
                            f"{location}: {locale} has {len(text)} Unicode code points, max_len is {max_len}"
                        )

            english_placeholders = set(PLACEHOLDER_PATTERN.findall(english))
            ukrainian_placeholders = set(PLACEHOLDER_PATTERN.findall(ukrainian))
            if ukrainian and english_placeholders != ukrainian_placeholders:
                raise CatalogError(
                    f"{location}: en/uk placeholders differ: "
                    f"{sorted(english_placeholders)} != {sorted(ukrainian_placeholders)}"
                )

            entries[key] = {
                "en": english,
                "uk": ukrainian,
                "namespace": namespace,
            }
            untranslated += ukrainian == ""

    return entries, untranslated


def _ts_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def _c_string(value: str) -> str:
    escaped: list[str] = []
    for character in value:
        codepoint = ord(character)
        if character == "\\":
            escaped.append("\\\\")
        elif character == '"':
            escaped.append('\\"')
        elif character == "\n":
            escaped.append("\\n")
        elif character == "\r":
            escaped.append("\\r")
        elif character == "\t":
            escaped.append("\\t")
        elif codepoint < 32 or codepoint == 127:
            escaped.append(f"\\{codepoint:03o}")
        else:
            escaped.append(character)
    return '"' + "".join(escaped) + '"'


def _render_typescript(entries: dict[str, dict[str, Any]]) -> str:
    keys = sorted(entries)
    lines = [
        "// generated — do not edit",
        "export const en = {",
    ]
    lines.extend(f"  {_ts_string(key)}: {_ts_string(entries[key]['en'])}," for key in keys)
    lines.extend(("} as const;", "", "export type I18nKey = keyof typeof en;", "", "export const uk: Record<I18nKey, string> = {"))
    lines.extend(f"  {_ts_string(key)}: {_ts_string(entries[key]['uk'])}," for key in keys)
    lines.extend(("} as const;", ""))
    return "\n".join(lines)


def _enum_names(keys: list[str]) -> dict[str, str]:
    used = {"UI_I18N_KEY_COUNT"}
    names: dict[str, str] = {}
    for key in keys:
        base = "UI_I18N_KEY_" + key.replace(".", "_").upper()
        candidate = base
        suffix = 2
        while candidate in used:
            candidate = f"{base}_{suffix}"
            suffix += 1
        names[key] = candidate
        used.add(candidate)
    return names


def _render_firmware(entries: dict[str, dict[str, Any]]) -> tuple[str, str]:
    keys = sorted(key for key, entry in entries.items() if entry["namespace"] in {"common", "tft"})
    names = _enum_names(keys)

    header = [
        "/* generated — do not edit */",
        "#ifndef UI_I18N_CATALOG_GEN_H",
        "#define UI_I18N_CATALOG_GEN_H",
        "",
        "typedef enum {",
    ]
    header.extend(f"    {names[key]} = {index}," for index, key in enumerate(keys))
    header.extend(
        (
            "    UI_I18N_KEY_COUNT",
            "} ui_i18n_key_t;",
            "",
            "const char *ui_i18n_catalog_en(ui_i18n_key_t key);",
            "const char *ui_i18n_catalog_uk(ui_i18n_key_t key);",
            "",
            "#endif",
            "",
        )
    )

    source = [
        "/* generated — do not edit */",
        '#include "ui_i18n_catalog.gen.h"',
        "",
        "static const char *const s_ui_i18n_en[UI_I18N_KEY_COUNT] = {",
    ]
    source.extend(f"    {_c_string(entries[key]['en'])}, /* {key} */" for key in keys)
    source.extend(("};", "", "static const char *const s_ui_i18n_uk[UI_I18N_KEY_COUNT] = {"))
    source.extend(f"    {_c_string(entries[key]['uk'])}, /* {key} */" for key in keys)
    source.extend(
        (
            "};",
            "",
            "const char *ui_i18n_catalog_en(ui_i18n_key_t key)",
            "{",
            "    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {",
            "        return 0;",
            "    }",
            "    return s_ui_i18n_en[(unsigned)key];",
            "}",
            "",
            "const char *ui_i18n_catalog_uk(ui_i18n_key_t key)",
            "{",
            "    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {",
            "        return 0;",
            "    }",
            "    return s_ui_i18n_uk[(unsigned)key];",
            "}",
            "",
        )
    )
    return "\n".join(header), "\n".join(source)


def generate_outputs(root: Path) -> tuple[dict[Path, str], int]:
    entries, untranslated = load_catalogs(root)
    firmware_header, firmware_source = _render_firmware(entries)
    return (
        {
            Path("webui/src/i18n/catalog.gen.ts"): _render_typescript(entries),
            Path("components/ui_i18n/include/ui_i18n_catalog.gen.h"): firmware_header,
            Path("components/ui_i18n/ui_i18n_catalog.gen.c"): firmware_source,
        },
        untranslated,
    )


def main(argv: list[str] | None = None, *, root: Path | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated files are stale; do not write")
    args = parser.parse_args(argv)
    repo_root = ROOT if root is None else root

    try:
        outputs, untranslated = generate_outputs(repo_root)
    except CatalogError as exc:
        print(exc, file=sys.stderr)
        return 1

    if args.check:
        stale = [
            relative.as_posix()
            for relative, content in outputs.items()
            if not (repo_root / relative).is_file()
            or (repo_root / relative).read_bytes() != content.encode("utf-8")
        ]
        if stale:
            print("stale generated files: " + ", ".join(stale), file=sys.stderr)
            result = 1
        else:
            print("generated files are current")
            result = 0
    else:
        for relative, content in outputs.items():
            path = repo_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content.encode("utf-8"))
            print(f"wrote {relative.as_posix()}")
        result = 0

    print(f"untranslated keys: {untranslated}")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
