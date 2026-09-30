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
CONCEPT_FIELDS = {"id", "tft", "web", "report", "rule", "equivalent_terms", "note"}
CONCEPT_RULES = {"same_uk", "equivalent"}
REQUIRED_CONCEPTS = {
    "recording", "stopping",
    "orientation.accepted", "orientation.moving", "orientation.tilted", "orientation.fault",
    "constrained", "unconstrained", "localization.unverified", "uncertainty",
    "power.battery", "power.charging", "power.full", "power.low", "power.critical", "power.unknown",
    "localization.solved", "localization.unsolved", "velocity.inconsistent", "velocity.at_bound",
}


class CatalogError(ValueError):
    """A source catalog is invalid."""


class _DuplicateJsonKey(ValueError):
    def __init__(self, key: str):
        self.key = key


def _read_json(path: Path, description: str = "catalog") -> Any:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise _DuplicateJsonKey(key)
            result[key] = value
        return result

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except _DuplicateJsonKey as exc:
        raise CatalogError(f"{path}: duplicate JSON object key {exc.key!r}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise CatalogError(f"{path}: cannot read {description}: {exc}") from exc


def load_catalogs(root: Path) -> tuple[dict[str, dict[str, Any]], int]:
    entries: dict[str, dict[str, Any]] = {}
    folded_keys: dict[str, str] = {}
    untranslated = 0

    for namespace in NAMESPACES:
        path = root / "i18n" / f"{namespace}.json"
        raw = _read_json(path)
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
            if not isinstance(key, str):
                raise CatalogError(f"{location}: key must use lowercase letters, digits, dots, and underscores")
            folded_key = key.casefold()
            previous_key = folded_keys.get(folded_key)
            if previous_key is not None:
                if previous_key == key:
                    raise CatalogError(f"{location}: duplicate key across catalogs")
                raise CatalogError(
                    f"{location}: case-insensitive key collision with {previous_key!r}"
                )
            if not KEY_PATTERN.fullmatch(key):
                raise CatalogError(f"{location}: key must use lowercase letters, digits, dots, and underscores")
            folded_keys[folded_key] = key
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


def _production_source_text(root: Path, surface: str) -> str:
    if not root.is_dir():
        return ""
    extensions = {".c", ".h"} if surface == "tft" else {".ts", ".tsx"}
    paths = sorted(root.rglob("*"))
    texts = []
    for source in paths:
        if not source.is_file() or source.suffix not in extensions:
            continue
        if surface == "tft":
            if ".gen." in source.name or any(
                part.casefold() == "tests" for part in source.relative_to(root).parts[:-1]
            ):
                continue
        elif source.name.endswith(".gen.ts"):
            continue
        texts.append(source.read_text(encoding="utf-8"))
    return "\n".join(texts)


def validate_concepts(
    root: Path,
    entries: dict[str, dict[str, Any]],
    *,
    source_roots: dict[str, Path] | None = None,
) -> None:
    path = root / "i18n" / "concepts.json"
    concepts = _read_json(path, "concept map")
    if not isinstance(concepts, list):
        raise CatalogError(f"{path}: concept map must be a list")

    roots = {
        "tft": root / "components",
        "web": root / "webui" / "src",
        "report": root / "webui" / "src",
    }
    if source_roots is not None:
        roots.update(source_roots)
    source_text = {surface: _production_source_text(roots[surface], surface) for surface in roots}

    seen: set[str] = set()
    for concept in concepts:
        if not isinstance(concept, dict):
            raise CatalogError(f"{path}: concept entry must be an object")
        unknown_fields = set(concept) - CONCEPT_FIELDS
        if unknown_fields:
            fields = ", ".join(sorted(unknown_fields))
            raise CatalogError(f"{path}: unknown concept field(s): {fields}")
        if set(concept) != CONCEPT_FIELDS:
            fields = ", ".join(sorted(CONCEPT_FIELDS - set(concept)))
            raise CatalogError(f"{path}: concept entry is missing field(s): {fields}")

        concept_id = concept["id"]
        if not isinstance(concept_id, str) or not concept_id.strip():
            raise CatalogError(f"{path}: concept id must be a non-empty string")
        if concept_id in seen:
            raise CatalogError(f"{path}: duplicate concept id {concept_id!r}")
        seen.add(concept_id)
        rule = concept["rule"]
        if not isinstance(rule, str) or rule not in CONCEPT_RULES:
            raise CatalogError(f"{path}: concept {concept_id!r} rule must be 'same_uk' or 'equivalent'")
        if not isinstance(concept["note"], str):
            raise CatalogError(f"{path}: concept {concept_id!r} note must be a string")
        equivalent_terms = concept["equivalent_terms"]
        if rule == "same_uk" and equivalent_terms is not None:
            raise CatalogError(f"{path}: concept {concept_id!r} same_uk rule must not declare equivalent_terms")
        if rule == "equivalent" and not isinstance(equivalent_terms, list):
            raise CatalogError(f"{path}: concept {concept_id!r} equivalent rule needs equivalent_terms")

        localized_values: list[tuple[str, str, str]] = []
        for surface in ("tft", "web", "report"):
            key = concept[surface]
            if key is None:
                continue
            if not isinstance(key, str) or not KEY_PATTERN.fullmatch(key):
                raise CatalogError(f"{path}: concept {concept_id!r} {surface} must be a catalog key or null")
            entry = entries.get(key)
            if entry is None:
                raise CatalogError(f"{path}: concept {concept_id!r} references missing {surface} key {key!r}")
            if entry["namespace"] != surface:
                raise CatalogError(
                    f"{path}: concept {concept_id!r} key {key!r} belongs to {entry['namespace']}, not {surface}"
                )
            localized_values.append((surface, entry["en"], entry["uk"]))

        if not any(concept[surface] is not None for surface in ("tft", "web", "report")):
            raise CatalogError(f"{path}: concept {concept_id!r} must reference at least one catalog key")
        if rule == "same_uk":
            for locale in ("en", "uk"):
                values = {english.casefold() if locale == "en" else ukrainian.casefold()
                          for _, english, ukrainian in localized_values}
                if len(values) > 1:
                    details = ", ".join(
                        f"{surface}={english if locale == 'en' else ukrainian!r}"
                        for surface, english, ukrainian in localized_values
                    )
                    raise CatalogError(f"{path}: concept {concept_id!r} same_uk {locale} values differ: {details}")
        else:
            allowed: set[tuple[str, str]] = set()
            for term in equivalent_terms:
                if not isinstance(term, dict) or set(term) != {"en", "uk"}:
                    raise CatalogError(f"{path}: concept {concept_id!r} equivalent_terms need en and uk pairs")
                if any(not isinstance(term[locale], str) or not term[locale].strip() for locale in ("en", "uk")):
                    raise CatalogError(f"{path}: concept {concept_id!r} equivalent_terms must be non-empty strings")
                allowed.add((term["en"].casefold(), term["uk"].casefold()))
            if not allowed:
                raise CatalogError(f"{path}: concept {concept_id!r} equivalent_terms must not be empty")
            for surface, english, ukrainian in localized_values:
                if (english.casefold(), ukrainian.casefold()) not in allowed:
                    raise CatalogError(
                        f"{path}: concept {concept_id!r} {surface} terms are outside declared equivalent_terms"
                    )

    by_id = {concept["id"]: concept for concept in concepts}
    for concept_id in sorted(REQUIRED_CONCEPTS):
        concept = by_id.get(concept_id)
        if concept is None:
            raise CatalogError(f"{path}: required concept {concept_id!r} is missing")
        if not any(concept[surface] is not None for surface in ("tft", "web", "report")):
            raise CatalogError(f"{path}: required concept {concept_id!r} must map at least one surface")

    for concept in concepts:
        concept_id = concept["id"]
        for surface in ("tft", "web", "report"):
            key = concept[surface]
            if key is None:
                continue
            if surface == "tft":
                token = "UI_I18N_KEY_" + key.replace(".", "_").upper()
                referenced = re.search(rf"\b{re.escape(token)}\b", source_text[surface]) is not None
            else:
                token = re.compile(r"(['\"])" + re.escape(key) + r"\1")
                referenced = token.search(source_text[surface]) is not None
            if not referenced:
                raise CatalogError(
                    f"{path}: concept {concept_id!r} {surface} key {key!r} is not referenced by production source"
                )


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


def _render_typescript_keys(entries: dict[str, dict[str, Any]]) -> str:
    keys = sorted(entries)
    lines = [
        "// generated — do not edit",
        "export const i18nKeys = [",
    ]
    lines.extend(f"  {_ts_string(key)}," for key in keys)
    lines.extend(("] as const;", "", "export type I18nKey = (typeof i18nKeys)[number];", ""))
    return "\n".join(lines)


def _render_typescript_locale(entries: dict[str, dict[str, Any]], locale: str) -> str:
    keys = sorted(entries)
    lines = [
        "// generated — do not edit",
        "import type { I18nKey } from './catalog.keys.gen';",
        "",
        f"export const {locale}: Record<I18nKey, string> = {{",
    ]
    lines.extend(f"  {_ts_string(key)}: {_ts_string(entries[key][locale])}," for key in keys)
    lines.extend(("};", ""))
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


def generate_outputs(
    root: Path,
    *,
    source_roots: dict[str, Path] | None = None,
) -> tuple[dict[Path, str], int]:
    entries, untranslated = load_catalogs(root)
    validate_concepts(root, entries, source_roots=source_roots)
    firmware_header, firmware_source = _render_firmware(entries)
    return (
        {
            Path("webui/src/i18n/catalog.keys.gen.ts"): _render_typescript_keys(entries),
            Path("webui/src/i18n/catalog.en.gen.ts"): _render_typescript_locale(entries, "en"),
            Path("webui/src/i18n/catalog.uk.gen.ts"): _render_typescript_locale(entries, "uk"),
            Path("components/ui_i18n/include/ui_i18n_catalog.gen.h"): firmware_header,
            Path("components/ui_i18n/ui_i18n_catalog.gen.c"): firmware_source,
        },
        untranslated,
    )


def main(
    argv: list[str] | None = None,
    *,
    root: Path | None = None,
    source_roots: dict[str, Path] | None = None,
) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if generated files are stale; do not write")
    args = parser.parse_args(argv)
    repo_root = ROOT if root is None else root

    try:
        outputs, untranslated = generate_outputs(repo_root, source_roots=source_roots)
    except CatalogError as exc:
        print(exc, file=sys.stderr)
        return 1

    if untranslated:
        print(f"untranslated keys: {untranslated}", file=sys.stderr)
        return 1

    if args.check:
        stale = [
            relative.as_posix()
            for relative, content in outputs.items()
            if not (repo_root / relative).is_file()
            or (repo_root / relative).read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            != content.encode("utf-8")
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
