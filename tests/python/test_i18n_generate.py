"""Schema, determinism, and stale-output checks for the i18n generator."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("i18n_generate", REPO_ROOT / "tools" / "i18n_generate.py")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)
NAMESPACES = ("common", "web", "report", "tft")


def write_fixture(root: Path) -> dict[str, dict]:
    data = {
        namespace: {
            "schema": 1,
            "namespace": namespace,
            "entries": {},
        }
        for namespace in NAMESPACES
    }
    data["common"]["entries"]["app.title"] = {"en": "GeoLocator", "uk": ""}
    for namespace, catalog in data.items():
        path = root / "i18n" / f"{namespace}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return data


def save_fixture(root: Path, data: dict[str, dict]) -> None:
    for namespace, catalog in data.items():
        (root / "i18n" / f"{namespace}.json").write_text(
            json.dumps(catalog, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )


@pytest.mark.parametrize(
    ("edit", "message"),
    [
        (lambda data: data["common"].update(schema=2), "schema must be 1"),
        (lambda data: data["common"].update(extra=True), "unknown catalog field"),
        (lambda data: data["common"].update(namespace="web"), "namespace must be 'common'"),
        (lambda data: data["common"].update(entries=[]), "entries must be an object"),
        (
            lambda data: data["common"]["entries"].update({"Bad.Key": {"en": "Bad", "uk": ""}}),
            "lowercase letters",
        ),
        (
            lambda data: data["common"]["entries"].update({"broken.entry": "not an object"}),
            "entry must be an object",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(fr="Titre"),
            "unknown locale or entry field",
        ),
        (
            lambda data: data["common"]["entries"].update({"new.entry": {"en": "Only English"}}),
            "both en and uk fields are required",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(en=42),
            "en must be a string",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(en=" \t "),
            "en must not be whitespace-only",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(en=""),
            "en must not be empty",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(uk="  "),
            "uk must not be whitespace-only",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(note=1),
            "note must be a string",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(max_len=8),
            "max_len is allowed only in the tft catalog",
        ),
        (
            lambda data: data["tft"]["entries"].update(
                {"menu.home": {"en": "Home", "uk": "", "max_len": True}}
            ),
            "max_len must be a positive integer",
        ),
        (
            lambda data: data["tft"]["entries"].update(
                {"menu.home": {"en": "Home", "uk": "", "max_len": None}}
            ),
            "max_len must be a positive integer",
        ),
        (
            lambda data: data["common"]["entries"]["app.title"].update(
                en="Open {count}", uk="Open {total}"
            ),
            "en/uk placeholders differ",
        ),
        (
            lambda data: data["tft"]["entries"].update(
                {"menu.home": {"en": "ééé", "uk": "", "max_len": 2}}
            ),
            "3 Unicode code points",
        ),
    ],
)
def test_rejects_invalid_catalog_fixture(tmp_path, edit, message):
    data = write_fixture(tmp_path)
    edit(data)
    save_fixture(tmp_path, data)

    with pytest.raises(generator.CatalogError, match=message):
        generator.generate_outputs(tmp_path)


def test_rejects_duplicate_keys_across_namespaces(tmp_path):
    data = write_fixture(tmp_path)
    data["web"]["entries"]["app.title"] = {"en": "Title", "uk": ""}
    save_fixture(tmp_path, data)

    with pytest.raises(generator.CatalogError, match="duplicate key across catalogs"):
        generator.generate_outputs(tmp_path)


def test_rejects_nonempty_uk_with_missing_or_extra_placeholders(tmp_path):
    data = write_fixture(tmp_path)
    data["common"]["entries"]["app.title"].update(en="Hello {name}", uk="Greeting")
    save_fixture(tmp_path, data)

    with pytest.raises(generator.CatalogError, match="en/uk placeholders differ"):
        generator.generate_outputs(tmp_path)


def test_firmware_catalog_contains_common_and_tft_namespaces_only():
    outputs, _ = generator.generate_outputs(REPO_ROOT)
    header = outputs[Path("components/ui_i18n/include/ui_i18n_catalog.gen.h")]

    assert "UI_I18N_KEY_APP_TITLE" in header
    assert "UI_I18N_KEY_MENU_OBJECTS" in header
    assert "UI_I18N_KEY_NAV_HOME" not in header
    assert "UI_I18N_KEY_REPORT_QR" not in header


def test_running_generator_twice_writes_identical_bytes(tmp_path):
    write_fixture(tmp_path)
    assert generator.main([], root=tmp_path) == 0
    paths = (
        Path("webui/src/i18n/catalog.gen.ts"),
        Path("components/ui_i18n/include/ui_i18n_catalog.gen.h"),
        Path("components/ui_i18n/ui_i18n_catalog.gen.c"),
    )
    first = {path: (tmp_path / path).read_bytes() for path in paths}

    assert generator.main([], root=tmp_path) == 0
    assert {path: (tmp_path / path).read_bytes() for path in paths} == first


def test_check_detects_stale_files_without_writing(tmp_path):
    write_fixture(tmp_path)
    assert generator.main([], root=tmp_path) == 0
    assert generator.main(["--check"], root=tmp_path) == 0

    stale = tmp_path / "webui/src/i18n/catalog.gen.ts"
    stale.write_bytes(b"stale\n")
    assert generator.main(["--check"], root=tmp_path) == 1
    assert stale.read_bytes() == b"stale\n"


def test_repository_catalogs_are_current():
    result = subprocess.run(
        [sys.executable, "tools/i18n_generate.py", "--check"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "untranslated keys:" in result.stdout
