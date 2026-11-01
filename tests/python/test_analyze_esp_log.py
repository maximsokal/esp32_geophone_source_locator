from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "esp_logs"

_spec = importlib.util.spec_from_file_location("analyze_esp_log", REPO_ROOT / "tools" / "analyze_esp_log.py")
assert _spec is not None and _spec.loader is not None
analyzer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = analyzer
_spec.loader.exec_module(analyzer)


def healthy_log(**override: str) -> str:
    """A 30 s acquisition window in which every gate is satisfied; parts can be replaced."""
    parts = {
        "start": "I (10000) AD7606_ACQ: Started: 20000 Hz, 4 channels, SPI3=10000000 Hz\n",
        "perf": "W (25000) UI: UI_PERF: target_fps=4 actual_fps=4.0 frames_rendered=40 frames_coalesced=1 "
                "max_render_us=90000 avg_render_us=60000\n",
        "rx": "".join(
            f"I ({t}) NRF_TOPOLOGY: NRF_LOG_RX node={n} addr=0x32 ch=0 requested=38 received=38\n"
            for t in range(10500, 40001, 1500) for n in (1, 2)),
        "topo": "I (20000) NRF_TOPOLOGY: generation=3 tca=yes assigned=4 conflicts=0 main_state=2 "
                "bus_busy=0 stale=0 age_ms=0\n",
        "rate": "I (39000) AD7606_RATE: AD7606_RATE: window_ms=1000 conv=20000 published=20000 attempts=20000 "
                "unstable=0 read_err=0 gaps=0 loops=20000 notify_max=1 isr=20000/300/900 loop=6000/9000 "
                "spi=3500/4000 pub=2000/3000 cpu_mhz=240 p2p=12/874/9683/2 rms10=45/1700/20000/9 "
                "mean=162/554/1193/-3 sat=0/0/0/0\n",
        "cpu": "I (40000) RUNTIME_OBSERVABILITY: RUNTIME_CPU stage=periodic window_ms=40000 valid=1 "
               "cpu0_busy=60.0 cpu0_idle=40.0 cpu1_busy=60.0 cpu1_idle=40.0\n"
               "I (40010) RUNTIME_OBSERVABILITY: RUNTIME_TASK rank=1 name=IDLE1 core=1 priority_current=0 "
               "priority_base=0 inherited=0 runtime_delta_us=15000000 cpu=37.5 stack_free=696\n",
        "stop": "I (40000) AD7606_ACQ: Stopped\n",
        "wdt": "",
    }
    parts.update(override)
    return "".join(parts.values())


def gates(text: str, **options) -> dict[str, str]:
    return analyzer.analyze_text(text, **options).gates()


def test_real_overload_log_fails_every_gate_with_measured_values():
    report = analyzer.analyze_file(FIXTURES / "acq_overload_2026-10-31_22-24-48.excerpt.log")
    # G6 is decided from the old SERIAL_DIAG fields (no AD7606_RATE line exists in this log); G7 has no data.
    assert report.gates() == {**{g: analyzer.FAIL for g in ("G1", "G2", "G3", "G4", "G5", "G6")},
                              "G0": analyzer.NOT_APPLICABLE, "G7": analyzer.NOT_APPLICABLE}  # excerpt has no boot lines
    g6 = [f for f in report.findings if f.gate == "G6"]
    assert len(g6) == 2 and all(f.status == analyzer.FAIL and "estimate" in f.measured for f in g6)
    assert "213 frames observed of 20692 conversions" in g6[0].measured
    assert [(w.start_ms, w.end_ms, w.rate_hz, w.channels) for w in report.windows] == [
        (33188, 66328, 20000, 4), (83808, 120688, 20000, 4)]
    wdt = [f for f in report.findings if f.gate == "G1"]
    # 8 events in total; one is glued to another line in the real log and must still be found.
    assert [f.measured for f in wdt] == ["4 task_wdt events", "4 task_wdt events"]
    g2 = [f for f in report.findings if f.gate == "G2"]
    assert all("min fps ratio 0.5" in f.measured for f in g2)
    nodes = {f.scope.split()[-1]: f for f in report.findings if f.gate == "G3" and f.scope.startswith("33.2s")}
    assert set(nodes) == {"node0", "node1", "node2", "node4"}
    assert int(nodes["node1"].measured.split()[2].rstrip("ms")) > 30000
    g4 = [f for f in report.findings if f.gate == "G4"]
    assert [f.status for f in g4] == [analyzer.FAIL, analyzer.FAIL]
    g5 = [float(f.measured.split()[2].rstrip("%")) for f in report.findings if f.gate == "G5"]
    assert len(g5) == 2 and all(0.5 <= value <= 2.0 for value in g5)
    assert report.adc_words["ch1"]["range"] == 12
    assert report.exit_code() == 1


def test_log_without_acquisition_is_not_applicable_never_pass():
    report = analyzer.analyze_file(FIXTURES / "no_acq_2026-10-31_16-24-32.excerpt.log")
    assert report.windows == []
    gates = report.gates()
    assert gates.pop("G0") == analyzer.NOT_APPLICABLE       # the excerpt keeps only the acquisition tags
    assert set(gates.values()) == {analyzer.NOT_APPLICABLE}
    assert report.exit_code() == 2


def test_healthy_window_passes_all_gates():
    report = analyzer.analyze_text(healthy_log())
    gates = report.gates()
    assert gates.pop("G0") == analyzer.NOT_APPLICABLE        # the synthetic log has no boot lines
    assert gates == {g: analyzer.PASS for g in ("G1", "G2", "G3", "G4", "G5", "G6", "G7")}
    assert report.exit_code() == 0


def test_delivery_and_core1_load_gates_use_the_firmware_rate_report():
    starved = healthy_log(rate="I (30000) AD7606_RATE: AD7606_RATE: window_ms=1000 conv=20000 published=213 "
                               "attempts=213 unstable=0 read_err=0 gaps=19787 loops=213 notify_max=400 "
                               "isr=20000/300/900 loop=6000/9000 spi=3500/4000 pub=2000/3000 cpu_mhz=240\n")
    result = gates(starved)
    assert result["G6"] == analyzer.FAIL and result["G7"] == analyzer.PASS
    saturated = healthy_log(rate="I (30000) AD7606_RATE: AD7606_RATE: window_ms=1000 conv=20000 published=20000 "
                                 "attempts=20000 unstable=0 read_err=0 gaps=0 loops=20000 notify_max=1 "
                                 "isr=20000/1500/4000 loop=9000/12000 spi=6000/8000 pub=2500/3000 cpu_mhz=240\n")
    result = gates(saturated)
    assert result["G7"] == analyzer.FAIL and result["G6"] == analyzer.PASS
    assert gates(healthy_log(rate=""))["G6"] == analyzer.NOT_APPLICABLE
    assert gates(healthy_log(rate=""))["G7"] == analyzer.NOT_APPLICABLE


def test_each_gate_fails_alone():
    wdt = "E (30000) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog\n"
    bad_perf = healthy_log(perf="W (25000) UI: UI_PERF: target_fps=4 actual_fps=2.1 frames_rendered=21 "
                                "frames_coalesced=1 max_render_us=400000 avg_render_us=380000\n")
    silent_node = healthy_log(rx="".join(
        f"I ({t}) NRF_TOPOLOGY: NRF_LOG_RX node=1 addr=0x32 ch=0 requested=38\n" for t in range(10500, 40001, 1500)
    ) + "I (10600) NRF_TOPOLOGY: NRF_LOG_RX node=2 addr=0x32 ch=0 requested=38\n")
    stale = healthy_log(topo="I (20000) NRF_TOPOLOGY: generation=3 tca=yes assigned=3 conflicts=0 "
                             "main_state=5 bus_busy=0 stale=1 age_ms=5800\n")
    expected = {
        "G1": healthy_log(wdt=wdt),
        "G2": bad_perf,
        "G3": silent_node,
        "G4": stale,
        "G5": healthy_log(cpu=(
            "I (40000) RUNTIME_OBSERVABILITY: RUNTIME_CPU stage=periodic window_ms=40000 valid=1 cpu0_busy=1.0\n"
            "I (40010) RUNTIME_OBSERVABILITY: RUNTIME_TASK rank=1 name=IDLE1 core=1 priority_current=0 "
            "runtime_delta_us=100000 cpu=0.3 stack_free=696\n")),
    }
    for failing, text in expected.items():
        result = gates(text)
        assert result[failing] == analyzer.FAIL, failing
        assert all(status == analyzer.PASS for gate, status in result.items() if gate not in (failing, "G0")), (failing, result)


def test_noise_bytes_and_glued_lines_do_not_hide_watchdog_events():
    base = healthy_log()
    glued = ("W (30000) sensor_bus: I2C_BUS_SUMMARY addr=0x32 dev=X ok=1 laE (30500) task_wdt: "
             "Task watchdog got triggered. The following tasks\n")
    noisy = base.replace("I (20000)", "\x1b[0;32mI (20000)").replace("stale=0 age_ms=0\n", "stale=0 age_ms=0\x1b[0m\n\x00\x00")
    text = analyzer.clean_text((noisy + glued).encode("utf-8"))
    report = analyzer.analyze_text(text)
    assert report.gate_status("G1") == analyzer.FAIL
    assert report.gate_status("G4") == analyzer.PASS


def test_optional_gates_report_not_applicable_when_evidence_is_missing():
    no_perf = healthy_log(perf="")
    no_cpu = healthy_log(cpu="")
    assert gates(no_perf)["G2"] == analyzer.NOT_APPLICABLE
    assert gates(no_cpu)["G5"] == analyzer.NOT_APPLICABLE
    assert gates(no_perf)["G1"] == analyzer.PASS


def test_window_without_stop_runs_to_the_last_timestamp():
    text = healthy_log(stop="")
    windows = analyzer.analyze_text(text).windows
    assert len(windows) == 1 and windows[0].end_ms == 40010


def test_cli_exit_codes_and_json(tmp_path, capsys):
    out = tmp_path / "report.json"
    code = analyzer.main([str(FIXTURES / "acq_overload_2026-10-31_22-24-48.excerpt.log"), "--json", str(out)])
    assert code == 1
    data = json.loads(out.read_text(encoding="utf-8"))
    assert data[0]["gates"]["G1"] == analyzer.FAIL
    assert analyzer.main([str(FIXTURES / "no_acq_2026-10-31_16-24-32.excerpt.log")]) == 2
    assert "NOT_APPLICABLE" in capsys.readouterr().out


def test_rate_report_channel_codes_are_reported_per_channel():
    report = analyzer.analyze_text(healthy_log())
    assert report.adc_rate_channels["ch1"]["p2p_max"] == 12     # the quiet channel is visible, not guessed
    assert report.adc_rate_channels["ch3"]["p2p_max"] == 9683
    assert report.adc_rate_channels["ch4"]["rms10_max"] == 9
    assert "info ch1 (AD7606_RATE): n=1 p2p=12..12" in analyzer.format_report(report)


def test_a_boot_that_never_reaches_the_application_fails_even_without_an_acquisition():
    # 2026-11-01: esptool lost the chip at 94%, the app partition was left half written and the ROM
    # bootloader looped on "invalid segment length 0xffffffff". Nothing else in the log looked wrong.
    report = analyzer.analyze_file(FIXTURES / "boot_failed_incomplete_flash_2026-11-01_13-02-20.excerpt.log")
    assert report.windows == []
    assert report.gate_status("G0") == analyzer.FAIL
    finding = next(f for f in report.findings if f.gate == "G0")
    assert "invalid segment length" in finding.measured and "No bootable app partitions" in finding.measured
    assert report.exit_code() == 1


def test_panics_fail_the_boot_gate_and_a_clean_start_passes_it():
    clean = "I (1200) app_init: Project name: x\nI (1300) cpu_start: Starting scheduler\n"
    assert analyzer.analyze_text(clean).gate_status("G0") == analyzer.PASS
    panic = clean + "Guru Meditation Error: Core  1 panic'ed (LoadProhibited)\n"
    assert analyzer.analyze_text(panic).gate_status("G0") == analyzer.FAIL
    assert analyzer.analyze_text("").gate_status("G0") == analyzer.NOT_APPLICABLE


def test_a_rate_line_cut_short_does_not_crash_the_analyzer():
    cut = healthy_log(rate="I (39000) AD7606_RATE: AD7606_RATE: window_ms=1000 conv=20000 published=20000 "
                           "attempts=20000 unstable=0 read_err=0 gaps=0 loops=20000 notify_max=1 "
                           "isr=20000/300/900 loop=6000/9000 spi=3500/4000 pub=2000/3000 cpu_mhz=240 "
                           "p2p=12/874/9683/2 rm\n")
    report = analyzer.analyze_text(cut)
    assert report.gate_status("G6") == analyzer.PASS
    assert report.adc_rate_channels["ch1"]["p2p_max"] == 12
    assert report.adc_rate_channels["ch1"]["rms10_max"] == 0


def test_an_intentional_restart_is_not_a_boot_failure():
    # esp_restart() and the safe-shutdown path print "Rebooting..."; only a panic or a failed boot must fail G0.
    clean = "I (1200) app_init: Project name: x\nI (1300) cpu_start: Starting scheduler\nRebooting...\n"
    assert analyzer.analyze_text(clean).gate_status("G0") == analyzer.PASS
    panic = clean + "Guru Meditation Error: Core  1 panic'ed (LoadProhibited)\nRebooting...\n"
    assert analyzer.analyze_text(panic).gate_status("G0") == analyzer.FAIL
