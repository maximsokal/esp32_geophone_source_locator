import importlib
import os
import sys
from pathlib import Path

from tools.idf_profile import parse_config_fragments, resolve_profile


REPO_ROOT = Path(__file__).resolve().parents[2]
PSRAM_DEFAULTS = (
    REPO_ROOT / "sdkconfig.defaults",
    REPO_ROOT / "tests" / "target" / "stress_app" / "sdkconfig.defaults",
    REPO_ROOT / "tests" / "target" / "hardware_self_test" / "sdkconfig.defaults",
)
PSRAM_SPEED_KEYS = {
    "CONFIG_SPIRAM_SPEED_80M",
    "CONFIG_SPIRAM_SPEED_40M",
    "CONFIG_SPIRAM_SPEED_120M",
    "CONFIG_SPIRAM_SPEED",
}
PROFILE_ENVIRONMENT = (
    "GEOLOCATOR_INSIGHTS_PROFILE",
    "GEOLOCATOR_KEYPAD_TIMING_PROFILE",
    "GEOLOCATOR_EMULATOR_PROFILE",
    "GEOLOCATOR_EMULATOR_WDT_PROFILE",
)
PROFILE_OVERLAYS = (
    "sdkconfig.emulator.defaults",
    "sdkconfig.emulator-wdt.defaults",
    "sdkconfig.profile-debug.defaults",
    "sdkconfig.profile-performance.defaults",
    "sdkconfig.insights.defaults",
)


def _assert_80mhz_octal_psram(settings, source: Path) -> None:
    assert settings.get("CONFIG_SPIRAM") is True, source
    assert settings.get("CONFIG_SPIRAM_MODE_OCT") is True, source
    assert settings.get("CONFIG_SPIRAM_SPEED_80M") is True, source
    assert settings.get("CONFIG_SPIRAM_SPEED_40M") is False, source
    assert settings.get("CONFIG_SPIRAM_SPEED_120M") is not True, source
    if "CONFIG_SPIRAM_SPEED" in settings:
        assert settings["CONFIG_SPIRAM_SPEED"] == "80", source


def _clear_profile_environment(monkeypatch) -> None:
    for variable in PROFILE_ENVIRONMENT:
        monkeypatch.delenv(variable, raising=False)


def test_committed_defaults_select_80mhz_octal_psram():
    for defaults in PSRAM_DEFAULTS:
        settings = parse_config_fragments([defaults])
        _assert_80mhz_octal_psram(settings, defaults)

    root_settings = parse_config_fragments([PSRAM_DEFAULTS[0]])
    assert root_settings.get("CONFIG_ESPTOOLPY_FLASHFREQ_80M") is True
    assert root_settings.get("CONFIG_ESPTOOLPY_FLASHFREQ_40M") is False
    assert root_settings.get("CONFIG_ESPTOOLPY_FLASHFREQ") == '"80m"'


def test_profile_overlays_do_not_override_psram_speed(monkeypatch):
    for overlay_name in PROFILE_OVERLAYS:
        overlay = REPO_ROOT / overlay_name
        overlay_settings = parse_config_fragments([overlay])
        assert PSRAM_SPEED_KEYS.isdisjoint(overlay_settings), overlay

    for profile in ("emulator", "emulator-wdt", "debug", "performance"):
        _clear_profile_environment(monkeypatch)
        resolved = resolve_profile(profile, REPO_ROOT)
        effective_settings = parse_config_fragments(
            [Path(path) for path in resolved["sdkconfig_defaults"]]
        )
        _assert_80mhz_octal_psram(effective_settings, Path(resolved["sdkconfig"]))

    _clear_profile_environment(monkeypatch)
    monkeypatch.setenv("GEOLOCATOR_INSIGHTS_PROFILE", "1")
    resolved = resolve_profile("performance", REPO_ROOT)
    effective_settings = parse_config_fragments(
        [Path(path) for path in resolved["sdkconfig_defaults"]]
    )
    assert REPO_ROOT / "sdkconfig.insights.defaults" in {
        Path(path) for path in resolved["sdkconfig_defaults"]
    }
    _assert_80mhz_octal_psram(effective_settings, Path(resolved["sdkconfig"]))


def _load_kconfiglib():
    try:
        return importlib.import_module("kconfiglib")
    except ModuleNotFoundError:
        pass

    idf_python_env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if not idf_python_env:
        return None

    env_root = Path(idf_python_env)
    site_packages = [env_root / "Lib" / "site-packages"]
    site_packages.extend(env_root.glob("lib/python*/site-packages"))
    for candidate in site_packages:
        if candidate.is_dir():
            sys.path.insert(0, str(candidate))
            try:
                return importlib.import_module("kconfiglib")
            except ModuleNotFoundError:
                sys.path.pop(0)
    return None


def _idf_spiram_kconfig() -> tuple[Path, Path] | None:
    candidates = []
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidates.append(Path(idf_path))
    candidates.append(Path(r"C:\Espressif\frameworks\esp-idf-v5.5.4"))

    for idf_root in candidates:
        kconfig_path = idf_root / "components" / "esp_psram" / "esp32s3" / "Kconfig.spiram"
        if kconfig_path.is_file():
            return idf_root, kconfig_path
    return None


def test_kconfiglib_resolves_80mhz_from_the_esp_idf_choice(tmp_path):
    kconfiglib = _load_kconfiglib()
    idf_sources = _idf_spiram_kconfig()
    if kconfiglib is None or idf_sources is None:
        return

    idf_root, spiram_kconfig = idf_sources
    top_level = tmp_path / "Kconfig"
    top_level.write_text(
        'mainmenu "ESP32-S3 PSRAM configuration contract"\n'
        f'source "{spiram_kconfig.as_posix()}"\n',
        encoding="utf-8",
    )

    previous_idf_path = os.environ.get("IDF_PATH")
    previous_srctree = os.environ.get("srctree")
    os.environ["IDF_PATH"] = str(idf_root)
    os.environ["srctree"] = str(REPO_ROOT)
    try:
        kconfig = kconfiglib.Kconfig(str(top_level), warn=False)
    finally:
        if previous_idf_path is None:
            os.environ.pop("IDF_PATH", None)
        else:
            os.environ["IDF_PATH"] = previous_idf_path
        if previous_srctree is None:
            os.environ.pop("srctree", None)
        else:
            os.environ["srctree"] = previous_srctree

    root_settings = parse_config_fragments([PSRAM_DEFAULTS[0]])
    kconfig.syms["SPIRAM"].set_value(2 if root_settings["CONFIG_SPIRAM"] else 0)
    kconfig.syms["SPIRAM_MODE_OCT"].set_value(
        2 if root_settings["CONFIG_SPIRAM_MODE_OCT"] else 0
    )
    kconfig.syms["SPIRAM_SPEED_80M"].set_value(
        2 if root_settings["CONFIG_SPIRAM_SPEED_80M"] else 0
    )
    kconfig.syms["SPIRAM_SPEED_40M"].set_value(
        2 if root_settings["CONFIG_SPIRAM_SPEED_40M"] else 0
    )

    assert kconfig.syms["SPIRAM_MODE_OCT"].str_value == "y"
    assert kconfig.syms["SPIRAM_SPEED_80M"].str_value == "y"
    assert kconfig.syms["SPIRAM_SPEED_40M"].str_value == "n"
    assert kconfig.syms["SPIRAM_SPEED"].str_value == "80"
