import hashlib
import importlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from shutil import copyfile, which

import pytest

from tools.idf_profile import (
    parse_config_fragments,
    resolve_profile,
    write_build_evidence,
    write_profile_cmake_include,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
PROFILE_RESOLVER = REPO_ROOT / "tools" / "idf_profile.py"
STRESS_APP_ROOT = REPO_ROOT / "tests" / "target" / "stress_app"


@pytest.mark.parametrize(
    ("profile", "selected", "unselected"),
    [
        ("debug", "CONFIG_COMPILER_OPTIMIZATION_DEBUG", "CONFIG_COMPILER_OPTIMIZATION_PERF"),
        ("performance", "CONFIG_COMPILER_OPTIMIZATION_PERF", "CONFIG_COMPILER_OPTIMIZATION_DEBUG"),
    ],
)
def test_profile_resolves_the_requested_optimization(profile, selected, unselected):
    resolved = resolve_profile(profile, REPO_ROOT)

    assert resolved["settings"][selected] is True
    assert resolved["settings"][unselected] is False


def test_wrong_profile_defaults_fixture_is_rejected(tmp_path):
    wrong_defaults = tmp_path / "sdkconfig.wrong-profile.defaults"
    wrong_defaults.write_text(
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y\n"
        "# CONFIG_COMPILER_OPTIMIZATION_DEBUG is not set\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="resolved defaults do not resolve to debug"):
        resolve_profile("debug", REPO_ROOT, profile_defaults=wrong_defaults)


def test_clean_profile_resolution_metadata_is_deterministic():
    first = resolve_profile("performance", REPO_ROOT)
    second = resolve_profile("performance", REPO_ROOT)

    assert first == second
    assert first["profile"] == "performance"
    assert first["build_dir"] == str(REPO_ROOT / "build-profile-performance")
    assert first["sdkconfig"] == str(REPO_ROOT / "build-profile-performance" / "sdkconfig")
    assert first["size_report"] == str(REPO_ROOT / "build-profile-performance" / "idf-size.txt")
    assert first["size_components_report"] == str(
        REPO_ROOT / "build-profile-performance" / "idf-size-components.txt"
    )


def test_emulator_profile_uses_only_emulator_settings_and_isolated_paths(monkeypatch):
    for variable in (
        "GEOLOCATOR_INSIGHTS_PROFILE",
        "GEOLOCATOR_KEYPAD_TIMING_PROFILE",
        "GEOLOCATOR_EMULATOR_PROFILE",
        "GEOLOCATOR_EMULATOR_WDT_PROFILE",
    ):
        monkeypatch.delenv(variable, raising=False)

    resolved = resolve_profile("emulator", REPO_ROOT)
    overlay = REPO_ROOT / "sdkconfig.emulator.defaults"

    assert resolved["profile"] == "emulator"
    assert resolved["optimization"] is None
    assert resolved["settings"]["CONFIG_APP_DISPLAY_ENABLED"] is False
    assert resolved["settings"]["CONFIG_APP_KEYPAD_ENABLED"] is False
    assert resolved["settings"]["CONFIG_SEISMIC_SOURCE_TEST"] is True
    assert resolved["cmake_profile_defaults"] == [str(overlay)]
    assert resolved["build_dir"] == str(REPO_ROOT / ".test-build" / "idf-emulator")
    assert resolved["sdkconfig"] == str(
        REPO_ROOT / ".test-build" / "idf-emulator" / "sdkconfig"
    )
    assert parse_config_fragments([overlay]) == {
        "CONFIG_APP_DISPLAY_ENABLED": False,
        "CONFIG_APP_KEYPAD_ENABLED": False,
        "CONFIG_SEISMIC_SOURCE_TEST": True,
        "CONFIG_APP_TEST_STARTUP_HEAP_PRESSURE": True,
    }
    cli_result = subprocess.run(
        [
            sys.executable,
            str(PROFILE_RESOLVER),
            "resolve",
            "--profile",
            "emulator",
            "--project",
            str(REPO_ROOT),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert json.loads(cli_result.stdout) == resolved


def test_emulator_wdt_profile_layers_emulator_then_wdt_and_isolates_build(monkeypatch):
    for variable in (
        "GEOLOCATOR_INSIGHTS_PROFILE",
        "GEOLOCATOR_KEYPAD_TIMING_PROFILE",
        "GEOLOCATOR_EMULATOR_PROFILE",
        "GEOLOCATOR_EMULATOR_WDT_PROFILE",
    ):
        monkeypatch.delenv(variable, raising=False)

    resolved = resolve_profile("emulator-wdt", REPO_ROOT)
    emulator_overlay = REPO_ROOT / "sdkconfig.emulator.defaults"
    wdt_overlay = REPO_ROOT / "sdkconfig.emulator-wdt.defaults"

    assert resolved["profile"] == "emulator-wdt"
    assert resolved["optimization"] is None
    assert resolved["settings"] == {
        "CONFIG_APP_DISPLAY_ENABLED": False,
        "CONFIG_APP_KEYPAD_ENABLED": False,
        "CONFIG_SEISMIC_SOURCE_TEST": True,
        "CONFIG_APP_TEST_WDT_TRIGGER": True,
        "CONFIG_APP_TEST_WDT_DELAY_S": "25",
    }
    assert resolved["cmake_profile_defaults"] == [
        str(emulator_overlay),
        str(wdt_overlay),
    ]
    assert resolved["sdkconfig_defaults"][-2:] == [
        str(emulator_overlay),
        str(wdt_overlay),
    ]
    assert resolved["build_dir"] == str(REPO_ROOT / ".test-build" / "idf-emulator-wdt")
    assert resolved["sdkconfig"] == str(
        REPO_ROOT / ".test-build" / "idf-emulator-wdt" / "sdkconfig"
    )
    assert parse_config_fragments([wdt_overlay]) == {
        "CONFIG_APP_TEST_WDT_TRIGGER": True,
        "CONFIG_APP_TEST_WDT_DELAY_S": "25",
    }

    cli_result = subprocess.run(
        [
            sys.executable,
            str(PROFILE_RESOLVER),
            "resolve",
            "--profile",
            "emulator-wdt",
            "--project",
            str(REPO_ROOT),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert json.loads(cli_result.stdout) == resolved


def test_emulator_wdt_profile_rejects_an_overlay_without_the_trigger(tmp_path):
    wrong_defaults = tmp_path / "sdkconfig.wrong-emulator-wdt.defaults"
    wrong_defaults.write_text(
        "CONFIG_APP_TEST_WDT_TRIGGER=n\n"
        "CONFIG_APP_TEST_WDT_DELAY_S=25\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="resolved defaults do not resolve to emulator-wdt"):
        resolve_profile("emulator-wdt", REPO_ROOT, profile_defaults=wrong_defaults)


def test_emulator_profile_rejects_defaults_that_enable_display_keypad_or_ad7606(tmp_path):
    wrong_defaults = tmp_path / "sdkconfig.wrong-emulator.defaults"
    wrong_defaults.write_text(
        "CONFIG_APP_DISPLAY_ENABLED=y\n"
        "CONFIG_APP_KEYPAD_ENABLED=y\n"
        "# CONFIG_SEISMIC_SOURCE_TEST is not set\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="resolved defaults do not resolve to emulator"):
        resolve_profile("emulator", REPO_ROOT, profile_defaults=wrong_defaults)


def _guarded_keypad_blocks(source: str) -> list[str]:
    return re.findall(r"(?ms)^#if APP_KEYPAD_ENABLED\s*$\n(.*?)^#endif\s*$", source)


def test_keypad_default_and_startup_source_gate_cover_runtime_consumers():
    kconfig = (REPO_ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    display_config = (REPO_ROOT / "components" / "display" / "include" / "app_display_config.h").read_text(encoding="utf-8")
    display_source = (REPO_ROOT / "components" / "display" / "display.c").read_text(encoding="utf-8")
    keypad_config = (REPO_ROOT / "main" / "app_keypad_config.h").read_text(encoding="utf-8")
    startup = (REPO_ROOT / "main" / "app_startup.c").read_text(encoding="utf-8")
    shutdown = (REPO_ROOT / "main" / "app_shutdown.c").read_text(encoding="utf-8")
    boot_ui = (REPO_ROOT / "main" / "app_storage_boot_ui.c").read_text(encoding="utf-8")
    storage_boot = (REPO_ROOT / "main" / "app_storage_boot.c").read_text(encoding="utf-8")
    status = (REPO_ROOT / "main" / "app_startup_status.c").read_text(encoding="utf-8")

    assert re.search(
        r"(?m)^config APP_DISPLAY_ENABLED\s*\n\s*bool[^\n]*\n\s*default y\s*$",
        kconfig,
    )
    normalized_kconfig = re.sub(r"\s+", " ", kconfig)
    assert "not modelled and remains unverified in the emulator" in normalized_kconfig
    assert "CONFIG_APP_DISPLAY_ENABLED" in display_config
    assert "#define APP_DISPLAY_ENABLED 1" in display_config
    assert "#define APP_DISPLAY_ENABLED 0" in display_config
    assert "#if APP_DISPLAY_ENABLED" in display_source
    assert "return ESP_ERR_NOT_SUPPORTED;" in display_source

    assert re.search(
        r"(?m)^config APP_KEYPAD_ENABLED\s*\n\s*bool[^\n]*\n\s*default y\s*$",
        kconfig,
    )
    assert "CONFIG_APP_KEYPAD_ENABLED" in keypad_config
    assert "app_display_config.h" in keypad_config
    assert "&& APP_DISPLAY_ENABLED" in keypad_config
    assert "#define APP_KEYPAD_ENABLED 1" in keypad_config
    assert "#define APP_KEYPAD_ENABLED 0" in keypad_config

    startup_blocks = _guarded_keypad_blocks(startup)
    assert any(
        "adc_service_init()" in block and "keypad_init()" in block
        for block in startup_blocks
    )
    assert any("keypad_start_event_monitor(" in block for block in startup_blocks)
    display_blocks = re.findall(r"(?ms)^#if APP_DISPLAY_ENABLED\s*$\n(.*?)^#endif\s*$", startup)
    assert any("app_storage_boot_ui_ensure_display_initialized()" in block for block in display_blocks)
    assert any("app_storage_boot_ui_ensure_initialized()" in block for block in display_blocks)
    assert any("ui_init()" in block for block in display_blocks)
    assert sum(block.count("ui_boot_set_stage(") for block in display_blocks) == 4
    assert any(
        "keypad_stop_event_monitor()" in block
        for block in _guarded_keypad_blocks(shutdown)
    )
    boot_ui_blocks = _guarded_keypad_blocks(boot_ui)
    assert any("keypad_start_boot_event_monitor(" in block for block in boot_ui_blocks)
    assert any("keypad_stop_boot_event_monitor()" in block for block in boot_ui_blocks)
    assert any(
        "app_storage_boot_set_recovery_ui(&s_recovery_ui);" in block
        and ".show_continue_without_storage" in block.split("#else", 1)[1]
        and "show_reason" not in block.split("#else", 1)[1]
        for block in boot_ui_blocks
    )
    status_blocks = _guarded_keypad_blocks(status)
    assert any("Keypad: OK" in block and "Keypad: SKIPPED" in block for block in status_blocks)
    status_display_blocks = re.findall(r"(?ms)^#if APP_DISPLAY_ENABLED\s*$\n(.*?)^#endif\s*$", status)
    assert any("Display: OK" in block and "Display: SKIPPED" in block for block in status_display_blocks)
    display_ui_blocks = re.findall(r"(?ms)^#if APP_DISPLAY_ENABLED\s*$\n(.*?)^#endif\s*$", boot_ui)
    assert any("display_init()" in block for block in display_ui_blocks)
    assert "return ESP_ERR_NOT_SUPPORTED;" in boot_ui.split(
        "esp_err_t app_storage_boot_ui_ensure_display_initialized(void)", 1
    )[1].split("esp_err_t app_storage_boot_ui_ensure_initialized(void)", 1)[0]
    shutdown_display_blocks = re.findall(r"(?ms)^#if APP_DISPLAY_ENABLED\s*$\n(.*?)^#endif\s*$", shutdown)
    assert any("display_set_enabled(" in block and "display_deinit()" in block for block in shutdown_display_blocks)
    assert any("ui_shutdown();" in block for block in shutdown_display_blocks)
    storage_boot_display_blocks = re.findall(r"(?ms)^#if APP_DISPLAY_ENABLED\s*$\n(.*?)^#endif\s*$", storage_boot)
    assert any("app_storage_boot_ui_ensure_initialized()" in block for block in storage_boot_display_blocks)
    assert any("ui_boot_show_error(message);" in block for block in storage_boot_display_blocks)
    diagnostic = storage_boot.split("void app_storage_boot_run_diagnostic(void)", 1)[1]
    assert diagnostic.index("app_storage_boot_continue_if_no_card(&input)") < diagnostic.index(
        "app_storage_boot_recover(input)"
    )


def test_emulator_profile_wiring_is_opt_in_and_forces_a_separate_test_build():
    cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    wrapper = (REPO_ROOT / "tools" / "run_idf.ps1").read_text(encoding="utf-8")
    profile_source = (REPO_ROOT / "tools" / "idf_profile.py").read_text(encoding="utf-8")
    defaults = (REPO_ROOT / "sdkconfig.emulator.defaults").read_text(encoding="utf-8")
    docs = (REPO_ROOT / "docs" / "firmware-test-plan.md").read_text(encoding="utf-8")

    assert "GEOLOCATOR_EMULATOR_PROFILE" in cmake
    assert "sdkconfig.emulator.defaults" in cmake
    assert ".test-build" in cmake and "message(FATAL_ERROR" in cmake
    assert '"emulator"' in wrapper
    assert ".test-build/idf-emulator" in wrapper
    assert '"emulator": ("sdkconfig.emulator.defaults",)' in profile_source
    assert "Never flash this configuration to hardware" in defaults
    assert "CONFIG_APP_DISPLAY_ENABLED=n" in defaults
    assert "not modelled" in docs and "unverified in the emulator" in docs
    assert "GEOLOCATOR_EMULATOR_WDT_PROFILE" in cmake
    assert "sdkconfig.emulator-wdt.defaults" in cmake
    assert '"emulator-wdt"' in wrapper
    assert ".test-build/idf-emulator-wdt" in wrapper
    assert "CONFIG_APP_TEST_WDT_TRIGGER=y" not in defaults
    assert "-Action build -Profile emulator-wdt" in docs
    assert "TEST_WDT: arming interrupt-watchdog test in <N> s" in docs
    assert "TEST_WDT: triggering interrupt watchdog now" in docs
    assert "TEST_WDT: skipped, reset reason is not POWERON (<name>)" in docs


def _resolve_app_test_wdt_kconfig(kconfiglib, scratch: Path):
    scratch.mkdir(parents=True, exist_ok=True)
    top_level = scratch / "Kconfig"
    main_kconfig = (REPO_ROOT / "main" / "Kconfig.projbuild").resolve()
    top_level.write_text(
        'mainmenu "App test watchdog contract"\n'
        f'source "{main_kconfig.as_posix()}"\n',
        encoding="utf-8",
    )

    previous_srctree = os.environ.get("srctree")
    os.environ["srctree"] = str(REPO_ROOT)
    try:
        return kconfiglib.Kconfig(str(top_level), warn=False)
    finally:
        if previous_srctree is None:
            os.environ.pop("srctree", None)
        else:
            os.environ["srctree"] = previous_srctree


def test_test_wdt_is_off_by_default_and_only_present_in_the_wdt_overlay(tmp_path):
    kconfig_text = (REPO_ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
    trigger_block = re.search(
        r"(?ms)^[ \t]*config APP_TEST_WDT_TRIGGER\s*\n(.*?)(?=^[ \t]*(?:config|endmenu)\b|\Z)",
        kconfig_text,
    )
    delay_block = re.search(
        r"(?ms)^[ \t]*config APP_TEST_WDT_DELAY_S\s*\n(.*?)(?=^[ \t]*(?:config|endmenu)\b|\Z)",
        kconfig_text,
    )
    assert trigger_block
    assert re.search(r"(?m)^[ \t]*bool\b", trigger_block.group(1))
    assert re.search(r"(?m)^[ \t]*default n\s*$", trigger_block.group(1))
    assert delay_block
    assert re.search(r"(?m)^[ \t]*range 5 600\s*$", delay_block.group(1))
    assert re.search(r"(?m)^[ \t]*default 25\s*$", delay_block.group(1))
    assert re.search(r"(?m)^[ \t]*depends on APP_TEST_WDT_TRIGGER\s*$", delay_block.group(1))

    for defaults in (
        REPO_ROOT / "sdkconfig.defaults",
        REPO_ROOT / "sdkconfig",
        REPO_ROOT / "sdkconfig.emulator.defaults",
    ):
        assert parse_config_fragments([defaults]).get("CONFIG_APP_TEST_WDT_TRIGGER") is not True

    kconfiglib = _load_kconfiglib()
    if kconfiglib is None:
        return

    kconfig = _resolve_app_test_wdt_kconfig(kconfiglib, tmp_path / "kconfig")
    trigger = kconfig.syms["APP_TEST_WDT_TRIGGER"]
    delay = kconfig.syms["APP_TEST_WDT_DELAY_S"]
    assert trigger.str_value == "n"
    trigger.set_value(2)
    assert trigger.str_value == "y"
    assert delay.str_value == "25"


def test_app_test_wdt_source_and_startup_hook_are_compile_time_gated():
    wdt_source = (REPO_ROOT / "main" / "app_test_wdt.c").read_text(encoding="utf-8")
    startup = (REPO_ROOT / "main" / "app_startup.c").read_text(encoding="utf-8")
    main_cmake = (REPO_ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")

    assert re.match(
        r'\s*#include "sdkconfig\.h"\s*\n\s*#if CONFIG_APP_TEST_WDT_TRIGGER\s*\n',
        wdt_source,
    )
    assert re.search(r"(?m)^#endif\s*$", wdt_source.rstrip())
    startup_blocks = re.findall(
        r"(?ms)^#if CONFIG_APP_TEST_WDT_TRIGGER\s*$\n(.*?)^#endif\s*$",
        startup,
    )
    assert any(re.search(r"(?m)^\s*app_test_wdt_start\(\);\s*$", block) for block in startup_blocks)
    assert startup.index("app_startup_log_success();") < startup.index("app_test_wdt_start();")
    cmake_gate = re.search(
        r"(?ms)if\(CONFIG_APP_TEST_WDT_TRIGGER\)(.*?)endif\(\)",
        main_cmake,
    )
    assert cmake_gate
    assert "app_test_wdt.c" in cmake_gate.group(1)
    assert "app_test_wdt_policy.c" in cmake_gate.group(1)

    start_references = []
    for source in (REPO_ROOT / "main").rglob("*"):
        if source.suffix.lower() not in {".c", ".h"}:
            continue
        if "app_test_wdt_start" in source.read_text(encoding="utf-8", errors="replace"):
            start_references.append(source.relative_to(REPO_ROOT).as_posix())
    assert set(start_references) == {
        "main/app_startup.c",
        "main/app_test_wdt.c",
        "main/app_test_wdt.h",
    }


def _backend_config_symbols() -> set[str]:
    symbols = set()
    source_extensions = {".c", ".h", ".cc", ".cpp", ".hpp"}
    components_root = REPO_ROOT / "components"
    for source in components_root.rglob("*"):
        if source.suffix.lower() not in source_extensions:
            continue
        if "tests" in source.relative_to(components_root).parts:
            continue
        text = source.read_text(encoding="utf-8", errors="replace")
        symbols.update(re.findall(r"CONFIG_(?:AD7606|SEISMIC)_[A-Z0-9_]+", text))
    return symbols


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


def _backend_kconfig_sources(main_kconfig: Path, config_symbols: set[str]) -> list[Path]:
    config_names = {symbol.removeprefix("CONFIG_") for symbol in config_symbols}
    component_kconfigs = sorted(
        path
        for path in (REPO_ROOT / "components").rglob("Kconfig*")
        if path.is_file()
    )
    sources = [main_kconfig]
    for source in component_kconfigs:
        declarations = set(
            re.findall(
                r"(?m)^\s*config\s+([A-Za-z0-9_]+)\b",
                source.read_text(encoding="utf-8", errors="replace"),
            )
        )
        if declarations.intersection(config_names):
            sources.append(source)
    return sources


def _check_backend_kconfig_structure(main_kconfig: Path, config_symbols: set[str]):
    kconfig_sources = _backend_kconfig_sources(main_kconfig, config_symbols)
    declared = set()
    source_text = []
    for source in kconfig_sources:
        text = source.read_text(encoding="utf-8", errors="replace")
        source_text.append(text)
        declared.update(
            re.findall(r"(?m)^\s*config\s+([A-Za-z0-9_]+)\b", text)
        )

    declared_config_symbols = {f"CONFIG_{name}" for name in declared}
    missing = sorted(config_symbols - declared_config_symbols)
    assert not missing, f"backend CONFIG symbols lack Kconfig definitions: {missing}"

    combined = "\n".join(source_text)
    for name, expected in (
        ("SEISMIC_AD7606_SAMPLE_RATE_HZ", "500"),
        ("AD7606_OVERSAMPLING_RATIO", "1"),
    ):
        block = re.search(
            rf"(?ms)^\s*config\s+{name}\s*\n(.*?)(?=^\s*(?:config|choice|menu|endmenu|endchoice)\b|\Z)",
            combined,
        )
        assert block, f"missing Kconfig block for {name}"
        defaults = re.findall(r"(?m)^\s*default\s+([^\n]+)", block.group(1))
        assert defaults and defaults[-1].strip() == expected, (
            f"{name} must end with unconditional fallback default {expected}"
        )


def _resolve_backend_kconfig(
    kconfiglib,
    main_kconfig: Path,
    selected_source: str,
    config_symbols: set[str],
    scratch: Path,
) -> dict[str, str]:
    kconfig_sources = _backend_kconfig_sources(main_kconfig, config_symbols)
    scratch.mkdir(parents=True, exist_ok=True)
    top_level = scratch / "Kconfig"
    top_level.write_text(
        'mainmenu "Backend CONFIG resolution contract"\n'
        + "".join(f'source "{source.resolve().as_posix()}"\n' for source in kconfig_sources),
        encoding="utf-8",
    )

    previous_srctree = os.environ.get("srctree")
    os.environ["srctree"] = str(REPO_ROOT)
    try:
        kconfig = kconfiglib.Kconfig(str(top_level), warn=False)
    finally:
        if previous_srctree is None:
            os.environ.pop("srctree", None)
        else:
            os.environ["srctree"] = previous_srctree

    kconfig.syms[selected_source].set_value(2)
    values = {}
    unresolved = {}
    for config_symbol in sorted(config_symbols):
        name = config_symbol.removeprefix("CONFIG_")
        symbol = kconfig.syms.get(name)
        if symbol is None or not symbol.nodes or not symbol.str_value:
            unresolved[config_symbol] = {
                "defined": bool(symbol and symbol.nodes),
                "value": symbol.str_value if symbol else "",
            }
        else:
            values[name] = symbol.str_value
    assert not unresolved, (
        f"backend CONFIG symbols do not resolve for {selected_source}: {unresolved}"
    )
    return values


def test_backend_config_symbols_resolve_for_test_and_ad7606_sources(tmp_path):
    config_symbols = _backend_config_symbols()
    assert config_symbols
    configured_main = Path(
        os.environ.get(
            "GEOLOCATOR_TEST_MAIN_KCONFIG",
            REPO_ROOT / "main" / "Kconfig.projbuild",
        )
    )
    kconfiglib = _load_kconfiglib()

    if kconfiglib is None:
        # Static fallback for test environments without the ESP-IDF Kconfiglib.
        # It checks declarations and the two previously empty numeric defaults;
        # real expression evaluation runs whenever Kconfiglib is available.
        _check_backend_kconfig_structure(configured_main, config_symbols)
        return

    ad7606_values = None
    for selected_source in ("SEISMIC_SOURCE_TEST", "SEISMIC_SOURCE_AD7606"):
        values = _resolve_backend_kconfig(
            kconfiglib,
            configured_main,
            selected_source,
            config_symbols,
            tmp_path / selected_source,
        )
        assert values[selected_source] == "y"
        if selected_source == "SEISMIC_SOURCE_TEST":
            # 500 Hz since 2026-11-01: 20 kHz delivered ~2% of the conversions (docs/acquisition-ui-load-plan-2026-11-01.md).
            assert values["SEISMIC_AD7606_SAMPLE_RATE_HZ"] == "500"
            assert values["AD7606_OVERSAMPLING_RATIO"] == "1"
        else:
            ad7606_values = values

    assert ad7606_values is not None
    assert ad7606_values["SEISMIC_AD7606_SAMPLE_RATE_HZ"] == "500"
    assert ad7606_values["AD7606_OVERSAMPLING_RATIO"] == "1"
    assert ad7606_values["AD7606_SPI_CLOCK_HZ"] == "10000000"
    assert ad7606_values["AD7606_READ_MODE_AFTER_CONVERSION"] == "n"
    assert ad7606_values["AD7606_READ_MODE_DURING_CONVERSION"] == "n"


@pytest.mark.parametrize(
    ("profile", "selected", "unselected", "wrong_profile"),
    [
        (
            "debug",
            "CONFIG_COMPILER_OPTIMIZATION_DEBUG",
            "CONFIG_COMPILER_OPTIMIZATION_PERF",
            "performance",
        ),
        (
            "performance",
            "CONFIG_COMPILER_OPTIMIZATION_PERF",
            "CONFIG_COMPILER_OPTIMIZATION_DEBUG",
            "debug",
        ),
    ],
)
def test_subproject_profile_uses_root_fragment_and_keeps_paths_isolated(
    profile, selected, unselected, wrong_profile, tmp_path, monkeypatch
):
    monkeypatch.chdir(tmp_path)
    resolved = resolve_profile(profile, STRESS_APP_ROOT)
    repeated = resolve_profile(profile, STRESS_APP_ROOT)
    profile_fragment = REPO_ROOT / f"sdkconfig.profile-{profile}.defaults"
    build_dir = STRESS_APP_ROOT / f"build-profile-{profile}"

    assert resolved == repeated
    assert resolved["sdkconfig_defaults"] == [
        str(STRESS_APP_ROOT / "sdkconfig.defaults"),
        str(profile_fragment),
    ]
    assert resolved["cmake_profile_defaults"] == [str(profile_fragment)]
    assert resolved["settings"][selected] is True
    assert resolved["settings"][unselected] is False
    assert resolved["build_dir"] == str(build_dir)
    assert resolved["sdkconfig"] == str(build_dir / "sdkconfig")
    assert resolved["evidence_json"] == str(
        build_dir / f"build-evidence.{profile}.json"
    )
    assert resolved["size_report"] == str(build_dir / "idf-size.txt")
    assert resolved["size_components_report"] == str(
        build_dir / "idf-size-components.txt"
    )

    wrong_fragment = REPO_ROOT / f"sdkconfig.profile-{wrong_profile}.defaults"
    with pytest.raises(
        ValueError, match=f"resolved defaults do not resolve to {profile}"
    ):
        resolve_profile(profile, STRESS_APP_ROOT, profile_defaults=wrong_fragment)


def test_subproject_profiles_use_separate_build_and_sdkconfig_paths():
    debug = resolve_profile("debug", STRESS_APP_ROOT)
    performance = resolve_profile("performance", STRESS_APP_ROOT)

    assert debug["build_dir"] != performance["build_dir"]
    assert debug["sdkconfig"] != performance["sdkconfig"]


def test_wrapper_resolution_cli_uses_the_same_defaults_and_isolated_sdkconfig():
    result = subprocess.run(
        [
            sys.executable,
            str(PROFILE_RESOLVER),
            "resolve",
            "--profile",
            "performance",
            "--project",
            str(REPO_ROOT),
            "--inherited-defaults",
            "sdkconfig.lvgl.defaults",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    resolved = json.loads(result.stdout)
    wrapper = (REPO_ROOT / "tools" / "run_idf.ps1").read_text(encoding="utf-8")

    assert resolved == resolve_profile(
        "performance", REPO_ROOT, inherited_defaults="sdkconfig.lvgl.defaults"
    )
    assert resolved["sdkconfig_defaults"] == [
        str(REPO_ROOT / "sdkconfig.defaults"),
        str(REPO_ROOT / "sdkconfig.storage.defaults"),
        str(REPO_ROOT / "sdkconfig.lvgl.defaults"),
        str(REPO_ROOT / "sdkconfig.tls.defaults"),
        str(REPO_ROOT / "sdkconfig.button.defaults"),
        str(REPO_ROOT / "sdkconfig.profile-performance.defaults"),
    ]
    assert '"prepare-cmake"' in wrapper and '"--profile"' in wrapper
    assert '$env:SDKCONFIG = $sdkconfigPath' in wrapper
    assert '"CMAKE_PROJECT_INCLUDE_BEFORE=$profileCmakeInclude"' in wrapper


def test_profile_cmake_include_appends_to_existing_project_defaults(tmp_path):
    profile_project = tmp_path / "profile-project"
    profile_project.mkdir()
    for name in (
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "sdkconfig.storage.defaults",
        "sdkconfig.lvgl.defaults",
        "sdkconfig.tls.defaults",
        "sdkconfig.button.defaults",
        "sdkconfig.profile-performance.defaults",
    ):
        copyfile(REPO_ROOT / name, profile_project / name)

    resolved = resolve_profile("performance", profile_project)
    include_path = write_profile_cmake_include(resolved)
    include_text = include_path.read_text(encoding="utf-8")

    assert "list(APPEND SDKCONFIG_DEFAULTS" in include_text
    assert (profile_project / "sdkconfig.profile-performance.defaults").as_posix() in include_text
    assert (profile_project / "sdkconfig.defaults").as_posix() not in include_text


def test_build_evidence_records_profile_and_firmware_sha(tmp_path):
    for name in (
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "sdkconfig.storage.defaults",
        "sdkconfig.lvgl.defaults",
        "sdkconfig.tls.defaults",
        "sdkconfig.button.defaults",
        "sdkconfig.profile-performance.defaults",
    ):
        copyfile(REPO_ROOT / name, tmp_path / name)
    build_dir = tmp_path / "build-profile-performance"
    build_dir.mkdir()
    firmware = build_dir / "geolocator.bin"
    firmware_bytes = b"synthetic firmware image for evidence contract"
    firmware.write_bytes(firmware_bytes)

    evidence_path = write_build_evidence("performance", tmp_path)
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))

    assert evidence["profile"] == "performance"
    assert evidence["firmware"] == firmware.name
    assert evidence["firmware_sha256"] == hashlib.sha256(firmware_bytes).hexdigest()


def test_invoke_checked_logs_stderr_and_checks_exit_code(tmp_path):
    powershell = which("powershell.exe") or which("powershell")
    if powershell is None:
        pytest.skip("Windows PowerShell is required")

    wrapper = (REPO_ROOT / "tools" / "run_idf.ps1").read_text(encoding="utf-8")
    start = wrapper.index("function Invoke-Checked(")
    end = wrapper.index("\n}", start) + 2
    success_log = tmp_path / "stderr-success.log"
    failure_log = tmp_path / "stderr-failure.log"
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    harness = tmp_path / "invoke-checked.ps1"
    harness.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        + wrapper[start:end]
        + "\n$successArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', \"[Console]::Error.WriteLine('stderr-success'); exit 0\")\n"
        + "$failureArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', 'exit 1')\n"
        + f"$successLog = {quote(success_log)}\n$failureLog = {quote(failure_log)}\n"
        + f"Invoke-Checked {quote(powershell)} $successArgs $successLog\n"
        + "if (-not (Select-String -Path $successLog -SimpleMatch -Pattern 'stderr-success' -Quiet)) { exit 32 }\n"
        + "$threw = $false\ntry { Invoke-Checked "
        + quote(powershell)
        + " $failureArgs $failureLog } catch { if ($_ -match 'exit code 1') { $threw = $true } }\n"
        + "if (-not $threw) { exit 33 }\nexit 0\n",
        encoding="utf-8",
    )

    result = subprocess.run(
        [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(harness)],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_emulator_rec_profile_layers_emulator_then_recording_and_isolates_build(monkeypatch):
    for variable in (
        "GEOLOCATOR_INSIGHTS_PROFILE",
        "GEOLOCATOR_KEYPAD_TIMING_PROFILE",
        "GEOLOCATOR_EMULATOR_PROFILE",
        "GEOLOCATOR_EMULATOR_WDT_PROFILE",
        "GEOLOCATOR_EMULATOR_REC_PROFILE",
    ):
        monkeypatch.delenv(variable, raising=False)

    resolved = resolve_profile("emulator-rec", REPO_ROOT)
    emulator_overlay = REPO_ROOT / "sdkconfig.emulator.defaults"
    rec_overlay = REPO_ROOT / "sdkconfig.emulator-rec.defaults"

    assert resolved["profile"] == "emulator-rec"
    assert resolved["settings"]["CONFIG_APP_TEST_RECORDING_SCENARIO"] is True
    assert resolved["cmake_profile_defaults"] == [str(emulator_overlay), str(rec_overlay)]
    assert resolved["build_dir"] == str(REPO_ROOT / ".test-build" / "idf-emulator-rec")
    # The shared emulator overlay (S1-S9) must stay free of the recording scenario and its partition table.
    shared = parse_config_fragments([emulator_overlay])
    assert "CONFIG_APP_TEST_RECORDING_SCENARIO" not in shared
    assert "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME" not in shared
    assert parse_config_fragments([rec_overlay])["CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"] == '"partitions.emulator.csv"'

