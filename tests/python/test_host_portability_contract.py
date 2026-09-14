from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


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
