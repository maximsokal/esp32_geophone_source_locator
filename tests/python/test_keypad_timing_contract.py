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


def test_timing_profiles_select_separate_sources_and_do_not_touch_production_defaults():
    root_cmake = read("CMakeLists.txt")
    timing_defaults = read("sdkconfig.keypad-timing.defaults")
    bench_defaults = read("sdkconfig.adc-bench.defaults")
    production_defaults = read("sdkconfig.defaults")
    runner = read("tools/run_keypad_timing_hil.ps1")
    timing_doc = read("docs/keypad-adc-timing-hil.md")

    assert "GEOLOCATOR_KEYPAD_TIMING_PROFILE" in root_cmake
    assert "GEOLOCATOR_ADC_BENCH_PROFILE" in root_cmake
    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS=y" in timing_defaults
    assert "CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH=y" in bench_defaults
    assert "CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ=500" in bench_defaults
    assert "CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS" not in production_defaults
    assert "-Sdkconfig $profileSdkconfig" in runner
    assert 'ValidateSet("AD7606", "ESP_ADC_BENCH")' in runner
    assert "GEOLOCATOR_KEYPAD_TIMING_PROFILE" in runner
    assert "GEOLOCATOR_ADC_BENCH_PROFILE" in runner
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
    assert "ESP ADC bench at 500 Hz while pressing" in timing_doc
    assert "bounded-histogram `p95`" in timing_doc
    assert "p95_overflow" in timing_doc
    assert "NOT_ESTABLISHED" in timing_doc
