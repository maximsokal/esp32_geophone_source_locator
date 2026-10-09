import importlib.util
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_host_runner_default_build_dir_uses_test_build(monkeypatch):
    runner_path = REPO_ROOT / "tools" / "run_host_tests.py"
    spec = importlib.util.spec_from_file_location("run_host_tests_default_path", runner_path)
    assert spec is not None and spec.loader is not None
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    monkeypatch.setattr(sys, "argv", [str(runner_path)])

    assert runner.parse_args().build_dir == Path(".test-build/host-tests")


def test_host_runner_gives_instrumented_runs_a_larger_timeout():
    runner_path = REPO_ROOT / "tools" / "run_host_tests.py"
    spec = importlib.util.spec_from_file_location("run_host_tests_timeout", runner_path)
    assert spec is not None and spec.loader is not None
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)

    plain = runner.run_timeout_seconds("none", False)
    assert plain == 60.0
    assert runner.run_timeout_seconds("address", False) > plain
    assert runner.run_timeout_seconds("undefined", False) > plain
    assert runner.run_timeout_seconds("address,undefined", False) > plain
    assert runner.run_timeout_seconds("none", True) > plain


def host_log_stubs() -> list[Path]:
    paths = list(REPO_ROOT.glob("components/*/tests/stubs/esp_log.h"))
    paths.extend(REPO_ROOT.glob("tests/host/stubs/**/esp_log.h"))
    return sorted(set(paths))


def test_host_log_stubs_avoid_gnu_variadic_comma_elision():
    offenders: list[str] = []

    for path in host_log_stubs():
        source = path.read_text(encoding="utf-8")
        if "##__VA_ARGS__" in source or "## __VA_ARGS__" in source:
            offenders.append(path.relative_to(REPO_ROOT).as_posix())

    assert not offenders, (
        "host C stubs must compile through the repository's strict C11/MSVC "
        "path without GNU variadic comma-elision: " + ", ".join(offenders)
    )


def test_manifest_host_log_stubs_preserve_all_log_arguments():
    paths = [
        "tests/host/stubs/status_indicator/esp_log.h",
        "tests/host/stubs/insights_bridge/esp_log.h",
        "tests/host/stubs/object_service/esp_log.h",
        "tests/host/stubs/storage_service/esp_log.h",
    ]

    for relative in paths:
        source = (REPO_ROOT / relative).read_text(encoding="utf-8")
        assert "host_esp_log_sink(__VA_ARGS__)" in source, relative
        assert "#define ESP_LOGI(tag, fmt, ...)" not in source, relative
        assert "#define ESP_LOGW(tag, fmt, ...)" not in source, relative
        assert "#define ESP_LOGE(tag, fmt, ...)" not in source, relative


def test_msvc_host_runner_keeps_strict_c11_warning_gate_and_real_atomics():
    source = (REPO_ROOT / "tools" / "run_host_tests.py").read_text(encoding="utf-8")

    assert '"/std:c11"' in source
    assert '"/W4"' in source
    assert '"/WX"' in source
    assert '"/UNDEBUG"' in source
    assert '"/D_CRT_SECURE_NO_WARNINGS"' in source
    assert '"/experimental:c11atomics"' in source
    assert "is_msvc_cl(compiler)" in source
    assert '"/Zc:preprocessor-"' not in source


def test_base_web_status_cache_suite_keeps_msvc_c11_atomics_enabled():
    manifest = (REPO_ROOT / "tests" / "host" / "manifest.json").read_text(encoding="utf-8")
    source = (
        REPO_ROOT / "components" / "web_server" / "web_status_cache_arbitration.c"
    ).read_text(encoding="utf-8")
    runner = (REPO_ROOT / "tools" / "run_host_tests.py").read_text(encoding="utf-8")

    # This production file is part of the base host suite, not only a coverage
    # shard. MSVC's C11 <stdatomic.h> support is opt-in, so losing the compiler
    # flag would make the ordinary Host C gate fail before any shard runs.
    assert '"name": "web_status_cache_arbitration"' in manifest
    assert '"components/web_server/web_status_cache_arbitration.c"' in manifest
    assert "#include <stdatomic.h>" in source
    assert "atomic_bool s_refresh_enabled" in source
    assert "atomic_store_explicit" in source
    assert "atomic_load_explicit" in source
    assert '"/experimental:c11atomics"' in runner
    assert "is_msvc_cl(compiler)" in runner


def test_insights_host_suite_compiles_production_atomics_with_portable_weak_hook():
    manifest = (REPO_ROOT / "tests" / "host" / "manifest.d" / "coverage-insights.json").read_text(
        encoding="utf-8"
    )
    source = (REPO_ROOT / "components" / "insights_bridge" / "insights_bridge.c").read_text(
        encoding="utf-8"
    )

    assert '"components/insights_bridge/insights_bridge.c"' in manifest
    assert '"CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS=0"' in manifest
    assert "#include <stdatomic.h>" in source
    assert "atomic_bool s_initialized" in source
    assert "atomic_bool s_retry_pending" in source
    assert "#define INSIGHTS_BRIDGE_WEAK __attribute__((weak))" in source
    assert "#define INSIGHTS_BRIDGE_WEAK\n" in source
    assert "app_esp_insights_auth_key(void) INSIGHTS_BRIDGE_WEAK;" in source
    assert "#undef INSIGHTS_BRIDGE_WEAK" in source


def test_powershell_scripts_avoid_c_integer_suffixes_on_windows_powershell_51():
    offenders: list[str] = []
    c_unsigned_literal = re.compile(r"(?<![A-Za-z0-9_])\d+[uU]\b")

    for path in sorted((REPO_ROOT / "tools").glob("*.ps1")):
        source = path.read_text(encoding="utf-8")
        if c_unsigned_literal.search(source):
            offenders.append(path.relative_to(REPO_ROOT).as_posix())

    assert not offenders, (
        "Windows PowerShell 5.1 does not accept C-style unsigned integer "
        "suffixes such as 0U: " + ", ".join(offenders)
    )


def test_object_store_host_path_does_not_require_posix_mode_t_on_msvc():
    source = (
        REPO_ROOT / "components" / "object_store" / "object_store_fs_ids.c"
    ).read_text(encoding="utf-8")

    assert "mode_t" not in source
    assert "_S_IFMT" in source
    assert "_S_IFDIR" in source
    assert "S_ISDIR(info.st_mode)" in source


def test_ui_navigation_host_fixture_reuses_production_internal_abi():
    stub = (
        REPO_ROOT / "tests" / "host" / "stubs" / "ui_navigation" / "ui_internal.h"
    ).read_text(encoding="utf-8")

    assert '#include "../../../../components/ui/ui_internal.h"' in stub
    assert "typedef struct" not in stub
    assert "ui_context_t" in stub  # only in the ABI-drift explanation comment
