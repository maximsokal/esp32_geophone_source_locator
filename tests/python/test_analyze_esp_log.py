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
                              "G0": analyzer.NOT_APPLICABLE, "G7": analyzer.NOT_APPLICABLE,  # excerpt has no boot lines
                              "G8": analyzer.NOT_APPLICABLE, "G9": analyzer.NOT_APPLICABLE}   # nor placement or ranging
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
    assert gates.pop("G8") == analyzer.NOT_APPLICABLE and gates.pop("G9") == analyzer.NOT_APPLICABLE
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
        # G8 and G9 (placement, ranging) have no lines in these synthetic acquisition logs.
        assert all(status == analyzer.PASS for gate, status in result.items() if gate not in (failing, "G0", "G8", "G9")), (failing, result)
        assert result["G8"] == analyzer.NOT_APPLICABLE and result["G9"] == analyzer.NOT_APPLICABLE


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


def placement_lines(stable_at_ms: int | None, start_ms: int = 44000, interruptions: int = 0) -> str:
    """PLACEMENT_STABILITY lines of MAIN and two nodes, as the firmware logs them (reason changes only)."""
    def line(t, node, stable, reason, trigger="change"):
        return (f"I ({t}) WORKFLOW: PLACEMENT_STABILITY node={node} stable={stable} reason={reason} trigger={trigger} "
                f"samples=1/8 range_mm=-1 limit_mm=100 age_ms=0 vertical=1 height_valid=1 height_mm=0\n")
    text = "".join(line(start_ms + i, node, 0, "collecting") for i, node in enumerate(("MAIN", "G1", "G2")))
    for k in range(interruptions):
        t = start_ms + 1000 * (k + 1)
        text += line(t, "MAIN", 0, "main_barometer") + line(t + 500, "MAIN", 0, "collecting")
    if stable_at_ms is not None:
        text += "".join(line(stable_at_ms + i, node, 1, "stable") for i, node in enumerate(("MAIN", "G1", "G2")))
    return text


def test_placement_gate_measures_the_time_until_every_node_is_stable():
    fast = analyzer.analyze_text(placement_lines(stable_at_ms=60000))     # 16 s after the first line
    g8 = [f for f in fast.findings if f.gate == "G8"]
    assert len(g8) == 1 and g8[0].status == analyzer.PASS
    assert "all nodes stable after 16.0s" in g8[0].measured and g8[0].scope == "placement 44.0s"

    # The last node turns stable two lines after the first stable line: exactly at the limit passes, a millisecond over fails.
    assert gates(placement_lines(stable_at_ms=44000 + 40000 - 2))["G8"] == analyzer.PASS
    assert gates(placement_lines(stable_at_ms=44000 + 40000 - 1))["G8"] == analyzer.FAIL


def test_placement_gate_names_the_barometer_interruptions_that_wiped_the_samples():
    # The bench log 2026-11-01 21:46: 10 interruptions, stable only after 76.8 s.
    slow = analyzer.analyze_text(placement_lines(stable_at_ms=44000 + 76800, interruptions=10))
    g8 = [f for f in slow.findings if f.gate == "G8"][0]
    assert g8.status == analyzer.FAIL
    assert "all nodes stable after 76.8s" in g8.measured and "10 MAIN barometer interruptions" in g8.measured


def test_placement_gate_fails_when_the_nodes_never_settle_and_is_silent_without_the_step():
    never = analyzer.analyze_text(placement_lines(stable_at_ms=None, interruptions=2) +
                                  "I (90000) WORKFLOW: WORKFLOW_COMMAND_DONE seq=9\n")
    g8 = [f for f in never.findings if f.gate == "G8"][0]
    assert g8.status == analyzer.FAIL and g8.measured.startswith("never settled in 46.0s")
    assert gates("I (1000) app_init: Project name: x\n")["G8"] == analyzer.NOT_APPLICABLE
    # A node that falls unstable again after the first moment of stability does not erase it.
    flap = placement_lines(stable_at_ms=60000) + "I (70000) WORKFLOW: PLACEMENT_STABILITY node=G1 stable=0 reason=range_exceeded trigger=change samples=8/8\n"
    assert gates(flap)["G8"] == analyzer.PASS


def ranging_lines(success: int, reason: str = "none", partial: int = 0) -> str:
    return (f"I (180000) WORKFLOW: RANGING_RESULT success={success} partial={partial} reason={reason} "
            "executions=10/12 pairs=6 residual_mm=74 max_residual_mm=74 inconsistent_pairs=0 main_reference_generation=9\n"
            "I (180001) WORKFLOW: RANGING_PAIR 0-1 state=valid distance_mm=2345 confidence=900 valid_samples=5 attempts=7 "
            "rejected=1 spread_mm=12 automatic_mm=2340\n")


def test_ranging_gate_decides_from_the_result_line_of_the_firmware():
    ok = analyzer.analyze_text(ranging_lines(1))
    g9 = [f for f in ok.findings if f.gate == "G9"][0]
    assert g9.status == analyzer.PASS and g9.measured == "success, 6 pairs"

    failed = analyzer.analyze_text(ranging_lines(0, "pair_failed:0-2", partial=1))
    g9 = [f for f in failed.findings if f.gate == "G9"][0]
    assert g9.status == analyzer.FAIL
    assert g9.measured == "failed: pair_failed:0-2 (partial pairs saved), executions 10/12"
    assert failed.exit_code() == 1

    # The last result of the log decides: a retry that succeeded makes it pass.
    retried = analyzer.analyze_text(ranging_lines(0, "timeout") + ranging_lines(1).replace("(180000)", "(260000)").replace("(180001)", "(260001)"))
    assert [f.status for f in retried.findings if f.gate == "G9"] == [analyzer.PASS]
    assert gates("I (1000) app_init: Project name: x\n")["G9"] == analyzer.NOT_APPLICABLE


def test_render_profile_and_memory_facts_are_reported_and_decide_no_gate():
    # Bench log 2026-11-01 21:46: "a chunk costs 20 ms" and "DMA fell to 175 bytes" had no numbers to explain them.
    facts = (
        "I (30000) UI: UI_PROFILE: frames=10 chunks_per_frame_x10=50 chunks_per_frame_max=12 chunk_draw_avg_us=2000 "
        "chunk_draw_max_us=9000 pixels_per_chunk=464 draw_ns_per_pixel=4310 scene_objects=120\n"
        "I (31000) UI: UI_PROFILE: frames=30 chunks_per_frame_x10=100 chunks_per_frame_max=12 chunk_draw_avg_us=14000 "
        "chunk_draw_max_us=20000 pixels_per_chunk=2400 draw_ns_per_pixel=5833 scene_objects=130\n"
        "W (31500) UI: UI_PROFILE: frames=0 chunks_per_frame_x10=0 chunks_per_frame_max=0 chunk_draw_avg_us=0 "
        "chunk_draw_max_us=0 pixels_per_chunk=0 draw_ns_per_pixel=0 scene_objects=130\n"
        "I (2179) UI_LVGL: UI_MEM_PROBE cpu_mhz=160 icache_bytes=16384 flash_mode=dio flash_mhz=80m psram_mhz=80 "
        "flash_cycles_per_read=190 psram_cycles_per_read=120 internal_cycles_per_read=4 region_bytes=131072\n"
        "I (35000) RUNTIME_OBSERVABILITY: MEM_WATCH dma_free=6071->175 delta=-5896 dma_largest=92 "
        "dma_min_since_boot=100 internal_free=7799 internal_min_since_boot=7000 tasks=31\n"
    )
    plain = analyzer.analyze_text(healthy_log())
    report = analyzer.analyze_text(healthy_log() + facts)
    assert report.gates() == plain.gates()                              # facts, not gates
    assert plain.render_profile == {} and plain.mem_probe == {} and plain.mem_watch == []

    profile = report.render_profile
    assert profile["reports"] == 2 and profile["frames"] == 40          # the empty window counts for nothing
    assert profile["chunks_per_frame_x10"] == 87                        # (10*50 + 30*100) / 40
    assert profile["chunk_draw_avg_us"] == 12285                        # weighted by chunks, not by windows
    assert profile["pixels_per_chunk"] == 2123
    assert profile["draw_ns_per_pixel"] == 5785
    assert profile["chunk_draw_max_us"] == 20000 and profile["scene_objects_max"] == 130
    assert report.mem_probe["flash_cycles_per_read"] == "190" and report.mem_probe["flash_mode"] == "dio"
    assert report.mem_watch == [{"t_ms": 35000, "from": 6071, "to": 175, "tasks": 31}]

    text = analyzer.format_report(report)
    assert "8.7 chunks/frame (max 12), 12.3 ms/chunk (max 20.0), 2123 px/chunk, 5785 ns/px" in text
    assert "MEM_WATCH 35.0s: dma_free 6071 -> 175 (-5896), 31 tasks" in text
    assert "flash_cycles_per_read=190" in text
