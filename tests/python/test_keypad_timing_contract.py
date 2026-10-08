"""Static contracts for the opt-in keypad timing measurement profile."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_timing_instrumentation_is_opt_in_and_stays_on_the_timer_task():
    adapter = read("components/keypad/keypad_button_adapter.c")
    stats = read("components/keypad/keypad_timing_stats.h")
    kconfig = read("main/Kconfig.projbuild")
    defaults = read("sdkconfig.keypad-timing.defaults")

    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS" in adapter
    assert "keypad_timing_probe" in adapter
    assert ".dispatch_method = ESP_TIMER_TASK" in adapter
    assert "KEYPAD_ADC_TIMING:" in adapter
    assert "min_us=%u avg_us=%u p95_us=%u" in adapter
    assert "p95_us=%u p95_overflow=%u p95_censored_above_us=%u max_us=%u" in adapter
    assert "p95_overflow=%u" in adapter
    assert "p95_censored_above_us=%u" in adapter
    assert "KEYPAD_TIMER_TIMING:" in adapter
    assert "overrun_count=%u" in adapter
    assert "samples at or above" in stats
    assert "const uint32_t normal_count" in stats
    assert "if (rank > normal_count)" in stats
    assert "(*overflow)++;\n        return;" in stats
    assert "histogram[bucket]++;" in stats
    assert "default n" in kconfig[kconfig.index("config APP_KEYPAD_TIMING_DIAGNOSTICS") :]
    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS=y" in defaults


def test_timing_profile_uses_the_ad7606_default_without_touching_production_defaults():
    root_cmake = read("CMakeLists.txt")
    timing_defaults = read("sdkconfig.keypad-timing.defaults")
    production_defaults = read("sdkconfig.defaults")
    kconfig = read("main/Kconfig.projbuild")
    runner = read("tools/run_keypad_timing_hil.ps1")
    profile_resolver = read("tools/idf_profile.py")
    timing_doc = read("docs/keypad-adc-timing-hil.md")

    assert "GEOLOCATOR_KEYPAD_TIMING_PROFILE" in root_cmake
    assert "GEOLOCATOR_ADC_BENCH_PROFILE" not in root_cmake
    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS=y" in timing_defaults
    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS" not in production_defaults
    assert "default SEISMIC_SOURCE_AD7606" in kconfig
    assert "CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH" not in kconfig
    assert "CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ" not in kconfig
    assert "-Sdkconfig $profileSdkconfig" in runner
    assert 'ValidateSet("AD7606")' in runner
    assert "GEOLOCATOR_KEYPAD_TIMING_PROFILE" in runner
    assert "GEOLOCATOR_ADC_BENCH_PROFILE" not in runner
    assert "GEOLOCATOR_ADC_BENCH_PROFILE" not in profile_resolver
    assert 'Add-Check $checks "Timing-profile flash"' in runner
    assert 'Add-Check $checks "Production flash"' not in runner
    assert "InvalidTelemetry" in runner
    assert "KEYPAD_TIMING_TELEMETRY_RESULT" in runner
    assert "PHYSICAL_CONDITION" in runner
    assert "KEYPAD_TIMING_RUN_RESULT" in runner
    assert "PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED" in runner
    assert "MinimumWindows" in runner
    assert "MinimumSnapshots" in runner
    assert "AD7606 with no deliberate keypad activity" in timing_doc
    assert "AD7606 while pressing the keypad repeatedly" in timing_doc
    assert "ESP ADC bench" not in timing_doc
    assert "bounded-histogram `p95`" in timing_doc
    assert "p95_overflow" in timing_doc
    assert "NOT_ESTABLISHED" in timing_doc


def test_removed_adc_bench_backend_has_no_unprotected_source_or_build_references():
    forbidden = (
        "ESP_ADC_BENCH",
        "geophone_adc_test",
        "SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ",
        "GEOLOCATOR_ADC_BENCH_PROFILE",
    )
    suffixes = {
        ".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".js", ".ts",
        ".tsx", ".py", ".ps1", ".cmake",
    }
    files = [REPO_ROOT / "CMakeLists.txt", *REPO_ROOT.glob("sdkconfig*.defaults")]
    for source_root in (REPO_ROOT / "components", REPO_ROOT / "main", REPO_ROOT / "tools"):
        files.extend(
            path
            for path in source_root.rglob("*")
            if path.is_file()
            and (
                path.suffix.lower() in suffixes
                or path.name == "CMakeLists.txt"
                or path.name.startswith("Kconfig")
            )
        )

    violations = []
    for path in sorted(set(files)):
        relative = path.relative_to(REPO_ROOT)
        content = path.read_text(encoding="utf-8")
        for token in forbidden:
            if token in content:
                violations.append(f"{relative}: {token}")

    assert not (REPO_ROOT / "components/geophone_adc_test").exists()
    assert not violations, "removed backend references remain:\n" + "\n".join(violations)
