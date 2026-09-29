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
    data["common"]["entries"]["app.title"] = {"en": "GeoLocator", "uk": "GeoLocator"}
    for namespace, catalog in data.items():
        path = root / "i18n" / f"{namespace}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (root / "i18n" / "concepts.json").write_text("[]\n", encoding="utf-8")
    return data


def save_fixture(root: Path, data: dict[str, dict]) -> None:
    for namespace, catalog in data.items():
        (root / "i18n" / f"{namespace}.json").write_text(
            json.dumps(catalog, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )


def save_concepts(root: Path, concepts: list[dict]) -> None:
    (root / "i18n" / "concepts.json").write_text(
        json.dumps(concepts, ensure_ascii=False, indent=2) + "\n",
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


def test_concept_map_rejects_a_missing_catalog_key(tmp_path):
    write_fixture(tmp_path)
    save_concepts(tmp_path, [{
        "id": "status.recording", "tft": None, "web": "missing.key", "report": None,
        "same_uk": False, "note": "Web key is not in its catalog.",
    }])

    with pytest.raises(generator.CatalogError, match="references missing web key"):
        generator.generate_outputs(tmp_path)
    assert generator.main(["--check"], root=tmp_path) == 1


def test_concept_map_rejects_a_same_uk_mismatch(tmp_path):
    data = write_fixture(tmp_path)
    for namespace, ukrainian in (("tft", "ЗАПИС"), ("web", "Триває"), ("report", "запис")):
        key = f"{namespace}.status.recording"
        data[namespace]["entries"][key] = {"en": "Recording", "uk": ukrainian}
    save_fixture(tmp_path, data)
    save_concepts(tmp_path, [{
        "id": "status.recording", "tft": "tft.status.recording", "web": "web.status.recording",
        "report": "report.status.recording", "same_uk": True, "note": "Expected parity.",
    }])

    with pytest.raises(generator.CatalogError, match="same_uk values differ"):
        generator.generate_outputs(tmp_path)


def test_concept_map_accepts_matching_uk_values_after_casefold(tmp_path):
    data = write_fixture(tmp_path)
    for namespace, ukrainian in (("tft", "ЗАПИС"), ("web", "Запис"), ("report", "запис")):
        key = f"{namespace}.status.recording"
        data[namespace]["entries"][key] = {"en": "Recording", "uk": ukrainian}
    save_fixture(tmp_path, data)
    save_concepts(tmp_path, [{
        "id": "status.recording", "tft": "tft.status.recording", "web": "web.status.recording",
        "report": "report.status.recording", "same_uk": True, "note": "Matching across surfaces.",
    }])

    generator.generate_outputs(tmp_path)


def test_repository_concept_map_covers_requested_state_families():
    concepts = json.loads((REPO_ROOT / "i18n" / "concepts.json").read_text(encoding="utf-8"))
    concept_ids = {concept["id"] for concept in concepts}
    assert {
        "recording", "stopping", "stopped", "review", "error",
        "orientation.accepted", "orientation.moving", "orientation.tilted", "orientation.fault",
        "power.battery", "power.charging", "power.full", "power.low", "power.critical", "power.unknown",
        "localization.solved", "localization.unsolved", "localization.unverified",
        "constrained", "unconstrained", "unknown", "unavailable",
    } <= concept_ids


def test_firmware_catalog_contains_common_and_tft_namespaces_only():
    outputs, _ = generator.generate_outputs(REPO_ROOT)
    header = outputs[Path("components/ui_i18n/include/ui_i18n_catalog.gen.h")]

    assert "UI_I18N_KEY_APP_TITLE" in header
    assert "UI_I18N_KEY_MENU_OBJECTS" in header
    assert "UI_I18N_KEY_NAV_HOME" not in header
    assert "UI_I18N_KEY_REPORT_QR" not in header


def test_running_generator_twice_writes_identical_bytes(tmp_path):
    write_fixture(tmp_path)
    paths = tuple(generator.generate_outputs(tmp_path)[0])
    assert generator.main([], root=tmp_path) == 0
    first = {path: (tmp_path / path).read_bytes() for path in paths}

    assert generator.main([], root=tmp_path) == 0
    assert {path: (tmp_path / path).read_bytes() for path in paths} == first


def test_check_detects_stale_files_without_writing(tmp_path):
    write_fixture(tmp_path)
    assert generator.main([], root=tmp_path) == 0
    assert generator.main(["--check"], root=tmp_path) == 0

    stale = tmp_path / next(iter(generator.generate_outputs(tmp_path)[0]))
    stale.write_bytes(b"stale\n")
    assert generator.main(["--check"], root=tmp_path) == 1
    assert stale.read_bytes() == b"stale\n"


def test_generator_refuses_to_generate_with_untranslated_keys(tmp_path, capsys):
    data = write_fixture(tmp_path)
    data["common"]["entries"]["app.title"]["uk"] = ""
    save_fixture(tmp_path, data)

    assert generator.main([], root=tmp_path) == 1
    assert "untranslated keys: 1" in capsys.readouterr().err
    assert not (tmp_path / "webui/src/i18n/catalog.en.gen.ts").exists()


def test_check_normalizes_crlf_but_detects_content_changes(tmp_path):
    for source in (REPO_ROOT / "i18n").glob("*.json"):
        target = tmp_path / "i18n" / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())

    outputs, _ = generator.generate_outputs(REPO_ROOT)
    for relative in outputs:
        current = (REPO_ROOT / relative).read_bytes()
        current_lf = current.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        assert current_lf == outputs[relative].encode("utf-8")
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(current_lf.replace(b"\n", b"\r\n"))

    assert generator.main(["--check"], root=tmp_path) == 0

    stale = tmp_path / next(iter(outputs))
    stale.write_bytes(stale.read_bytes() + b"// content changed\r\n")
    assert generator.main(["--check"], root=tmp_path) == 1


def test_repository_catalogs_are_current():
    result = subprocess.run(
        [sys.executable, "tools/i18n_generate.py", "--check"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "untranslated keys: 0" in result.stdout


def test_common_catalog_uk_values_are_nonempty_and_brand_is_explicitly_invariant():
    catalog = json.loads((REPO_ROOT / "i18n" / "common.json").read_text(encoding="utf-8"))
    for key, entry in catalog["entries"].items():
        assert entry["uk"].strip() or entry["en"] == entry["uk"], key

    assert catalog["entries"]["app.title"]["en"] == catalog["entries"]["app.title"]["uk"]


NEW_TRANSLATION_KEYS = {
    "web": (
        "web.error_prefix",
        "api.error_saved_session_required",
        "api.error_request_timeout",
        "api.error_request_failed",
        "api.error_http",
        "api.scan_save_timeout",
        "api.scan_save_failed",
        "api.scan_save_unaccepted",
        "api.scan_load_timeout",
        "api.scan_load_failed",
        "api.scan_info_failed",
        "console.error.object_store_unavailable",
        "console.error.transition_failed",
        "console.error.phone_gps_not_saved",
        "console.error.recorder_failed",
        "console.error.geolocation_unsupported",
        "console.error.measurement_failed",
        "console.error.status_unavailable",
        "console.error.cache_timeout",
        "console.error.transition_status_unavailable",
        "console.error.transition_timeout",
        "console.error.acquisition_unavailable",
        "console.error.gps_unavailable",
        "scan.error.no_stable_surface",
        "scan.error.restore_failed",
        "scan.error.prepare_failed",
        "scan.error.continue_unavailable",
        "scan.error.continue_base_missing",
        "scan.error.continue_not_saved",
        "scan.error.no_object_selected",
        "scan.storage.preempted",
        "calc.error.request_failed",
        "xr.error.secure_context",
        "xr.error.webxr_unavailable",
        "xr.error.layer_unavailable",
        "xr.error.webgl_unavailable",
        "xr.error.gpu_depth_only",
        "xr.error.cpu_depth_unavailable",
        "xr.error.depth_api_unavailable",
        "xr.error.depth_access_failed",
    ),
    "report": ("report.qr_too_long",),
}


def test_new_translation_keys_have_matching_en_uk_placeholders():
    for namespace, keys in NEW_TRANSLATION_KEYS.items():
        entries = json.loads((REPO_ROOT / "i18n" / f"{namespace}.json").read_text(encoding="utf-8"))["entries"]
        for key in keys:
            assert key in entries, f"missing {namespace} translation key {key}"
            entry = entries[key]
            assert set(generator.PLACEHOLDER_PATTERN.findall(entry["en"])) == set(
                generator.PLACEHOLDER_PATTERN.findall(entry["uk"])
            ), key
