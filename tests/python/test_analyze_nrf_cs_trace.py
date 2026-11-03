from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("analyze_nrf_cs_trace", ROOT / "tools" / "analyze_nrf_cs_trace.py")
assert _spec is not None and _spec.loader is not None
tool = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = tool
_spec.loader.exec_module(tool)

# Two attempts of the bench log 2026-11-02 12:55 (MAIN): connected after 361 and 250 ms, cut in SECURING; and a made-up
# one that ran through, to check the stage arithmetic.
LOG = """\
twis: noise that is not a trace line
cs: t=52011 a=1 execute init=0 refl=1 try=1 pair=0 budget=500ms
cs: t=52011 a=1 ev=EXECUTE_WORK_STARTED status=0 phase=IDLE +0ms
cs: t=52372 a=1 ev=CONNECTED status=0 phase=CONNECTING +361ms
cs: t=52511 a=1 TIMEOUT phase=SECURING budget=500ms +500ms
cs: t=52536 a=1 ev=TRANSPORT_ERROR status=9 phase=CLEANUP +525ms
cs: t=52536 a=1 result=TIMEOUT dist_mm=0 conf=0 phase=IDLE +525ms
cs: t=53415 a=3 execute init=0 refl=1 try=3 pair=0 budget=500ms
cs: t=53415 a=3 ev=EXECUTE_WORK_STARTED status=0 phase=IDLE +0ms
cs: t=53665 a=3 ev=CONNECTED status=0 phase=CONNECTING +250ms
cs: t=53915 a=3 TIMEOUT phase=SECURING budget=500ms +500ms
cs: t=55754 a=0 ev=INCOMING_CONNECTED status=0 phase=IDLE
cs: t=56095 a=0 ev=INCOMING_DISCONNECTED status=0 phase=IDLE
cs: t=60000 a=7 execute init=0 refl=2 try=1 pair=2 budget=4000ms
cs: t=60300 a=7 ev=CONNECTED status=0 phase=CONNECTING +300ms
cs: t=60900 a=7 ev=SECURITY_READY status=0 phase=SECURING +900ms
cs: t=61500 a=7 ev=MTU_READY status=0 phase=MTU +1500ms
cs: t=63000 a=7 result=SUCCESS dist_mm=3420 conf=870 phase=PROCEDURE +3000ms
"""


def test_stage_times_are_the_gaps_between_the_events_of_an_attempt():
    attempts, links = tool.parse(LOG)
    assert sorted(attempts) == [1, 3, 7]
    assert attempts[1].timeout_phase == "SECURING" and attempts[1].result == "TIMEOUT" and attempts[1].total_ms == 525
    assert attempts[7].budget_ms == 4000 and attempts[7].result == "SUCCESS"
    stages = tool.stage_times(attempts)
    assert sorted(stages["CONNECTED"]) == [250, 300, 361]
    assert stages["SECURITY_READY"] == [600] and stages["MTU_READY"] == [600]      # 900-300 and 1500-900
    assert "EXECUTE_WORK_STARTED" not in stages and "TRANSPORT_ERROR" not in stages
    assert links == [341]                                                          # a reflector-side link


def test_a_budget_is_suggested_only_from_attempts_that_succeeded():
    attempts, links = tool.parse(LOG)
    text = "\n".join(tool.report("log", attempts, links))
    assert "cut in phase: SECURING=2" in text
    assert "successful attempts took max 3000 ms: budget with margin 4500 ms" in text
    failed_only = {k: v for k, v in attempts.items() if v.result != "SUCCESS"}
    text = "\n".join(tool.report("log", failed_only, []))
    assert "no attempt succeeded: no budget can be derived" in text
    assert 'no "cs:" lines' in "\n".join(tool.report("log", {}, []))


# Three attempts at a pair 150 mm apart (the owner's bench 2026-11-02: nodes 10..20 cm apart, the library said 1.5..2 m):
# the IFFT estimate sits ~1.6 m off, the phase slope is closer. The log says which method deserves trust.
ACCURACY_LOG = """cs: t=100 a=1 execute init=4 refl=0 try=1 pair=5 budget=2400ms
cs: t=900 a=1 est ifft_mm=1750 phase_mm=240 rtt_mm=n/a best_mm=1750
cs: t=100 a=2 execute init=0 refl=4 try=1 pair=4 budget=2400ms
cs: t=900 a=2 est ifft_mm=1790 phase_mm=100 rtt_mm=2200 best_mm=1790
cs: t=100 a=3 execute init=1 refl=2 try=1 pair=1 budget=2400ms
cs: t=900 a=3 est ifft_mm=1500 phase_mm=300 rtt_mm=n/a best_mm=1500
"""


def test_the_estimates_of_each_method_are_compared_with_the_true_distances():
    attempts, _ = tool.parse(ACCURACY_LOG)
    assert attempts[1].pair == (0, 4) and attempts[2].pair == (0, 4)       # the direction does not matter
    assert attempts[1].estimates == {"ifft": 1750, "phase": 240, "best": 1750}   # "n/a" is no estimate
    truth = tool.parse_truth("4-0=150,1-2=200")
    assert truth == {(0, 4): 150, (1, 2): 200}
    lines = tool.accuracy(attempts, truth)
    by_method = {line.split()[0]: line for line in lines}
    # errors: ifft 1600, 1640, 1300 (mean 1513); phase 90, -50, 100 (mean 47); rtt 2050 (one estimate)
    assert "n=3" in by_method["ifft"] and "offset=    1513 mm" in by_method["ifft"]
    assert "n=3" in by_method["phase"] and "offset=      47 mm" in by_method["phase"]
    assert "n=1" in by_method["rtt"] and "offset=    2050 mm" in by_method["rtt"]
    assert "worst after removing the offset +-97 mm" in by_method["phase"]     # |-50 - 47|
    assert "no estimates at the given pairs" in "".join(tool.accuracy(attempts, {(5, 6): 100}))


def test_the_report_prints_the_accuracy_only_when_the_truth_is_given():
    attempts, links = tool.parse(ACCURACY_LOG)
    assert "accuracy against" not in nl_join(tool.report("log", attempts, links))
    assert "accuracy against" in nl_join(tool.report("log", attempts, links, tool.parse_truth("0-4=150")))


def nl_join(lines):
    return chr(10).join(lines)


# MAIN attempts of the bench log 2026-11-02 20:12 in the format of the firmware that names the method and the timing:
# attempt 1 is a normal one, attempt 2 has the distance at +1268 ms but the disconnect only at +5262 ms with the budget
# timer fired during cleanup, attempt 3 is a pairing that failed in SECURING (Zephyr status 9).
METHOD_LOG = """\
cs: t=153047 a=1 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=153433 a=1 est ifft_mm=6272 phase_mm=8917 rtt_mm=9550 best_mm=6272 method=ifft
cs: t=153484 a=1 result=SUCCESS dist_mm=6272 conf=953 phase=IDLE measured=1428ms cleanup=53ms +1481ms
cs: t=153652 a=2 execute init=0 refl=1 try=2 pair=0 budget=2400ms
cs: t=154921 a=2 est ifft_mm=6194 phase_mm=8514 rtt_mm=10568 best_mm=6194 method=ifft
cs: t=156052 a=2 TIMEOUT phase=CLEANUP budget=2400ms +2400ms
cs: t=158914 a=2 result=SUCCESS dist_mm=6194 conf=755 phase=IDLE measured=1268ms cleanup=3994ms over_budget=1 timeout_in_cleanup=1 +5262ms
cs: t=159107 a=3 execute init=0 refl=1 try=3 pair=0 budget=2400ms
cs: t=159340 a=3 ev=TRANSPORT_ERROR status=9 phase=SECURING err=SECURITY_UNSPECIFIED +233ms
cs: t=159340 a=3 result=TRANSPORT_ERROR dist_mm=0 conf=0 phase=IDLE measured=233ms cleanup=0ms +233ms
cs: t=245010 a=6 execute init=0 refl=4 try=1 pair=4 budget=2400ms
cs: t=246303 a=6 est ifft_mm=n/a phase_mm=4638 rtt_mm=3164 best_mm=4638 method=phase
cs: t=246337 a=6 result=SUCCESS dist_mm=4638 conf=872 phase=IDLE measured=1300ms cleanup=34ms +1334ms
"""


def test_the_method_behind_every_distance_is_counted():
    attempts, _ = tool.parse(METHOD_LOG)
    assert attempts[1].method == "ifft" and attempts[6].method == "phase" and attempts[3].method is None
    assert dict(tool.methods_used(attempts)) == {"ifft": 2, "phase": 1}      # the failed attempt has no est line
    text = nl_join(tool.report("log", attempts, []))
    assert "method used for the distance: ifft=2, phase=1" in text
    # old firmware: no method= in the est line, the methods are "unknown", not guessed from the numbers
    old_attempts, _ = tool.parse(ACCURACY_LOG)
    assert dict(tool.methods_used(old_attempts)) == {"unknown": 3}


def test_the_estimates_of_one_procedure_are_compared_with_each_other():
    attempts, _ = tool.parse(METHOD_LOG)
    # attempt 1: 9550-6272 = 3278; attempt 2: 10568-6194 = 4374; attempt 6: 4638-3164 = 1474 (ifft is not available)
    assert sorted(tool.estimate_spans(attempts)) == [1474, 3278, 4374]
    text = nl_join(tool.report("log", attempts, []))
    assert "span between the estimates of one procedure: min=1474 median=3278 max=4374 mm; above 1000 mm in 3 of 3" in text
    # one estimate alone cannot be compared with anything
    single, _ = tool.parse(chr(10).join([
        "cs: t=1 a=1 execute init=0 refl=1 try=1 pair=0 budget=2400ms",
        "cs: t=2 a=1 est ifft_mm=100 phase_mm=n/a rtt_mm=n/a best_mm=100 method=ifft",
    ]))
    assert single[1].estimates == {"ifft": 100, "best": 100}
    assert tool.estimate_spans(single) == []


def test_a_late_cleanup_is_listed_apart_from_a_timeout():
    attempts, _ = tool.parse(METHOD_LOG)
    assert (attempts[1].measured_ms, attempts[1].cleanup_ms, attempts[1].over_budget) == (1428, 53, False)
    assert (attempts[2].measured_ms, attempts[2].cleanup_ms) == (1268, 3994)
    assert attempts[2].over_budget and attempts[2].timeout_in_cleanup and attempts[2].result == "SUCCESS"
    assert not attempts[1].timeout_in_cleanup
    text = nl_join(tool.report("log", attempts, []))
    assert "attempts past their budget: 1 (budget timer fired during cleanup in 1), longest cleanup 3994 ms" in text
    assert "cut in phase: CLEANUP=1" in text        # the stage list still sees the timer, the line above explains it


def test_transport_errors_are_counted_by_the_reason_the_firmware_names():
    attempts, _ = tool.parse(METHOD_LOG)
    assert attempts[3].error_reasons == ["SECURITY_UNSPECIFIED"]
    assert "transport errors by reason: SECURITY_UNSPECIFIED=1" in nl_join(tool.report("log", attempts, []))
    clean = {k: v for k, v in attempts.items() if k != 3}
    assert "transport errors by reason" not in nl_join(tool.report("log", clean, []))


def test_the_scatter_of_each_pair_shows_the_noisy_links():
    # 0->4 of the 20:12 log: 3.8 .. 8.5 m (sd 1.8 m) while 3->4 of the 20:22 log stays within 0.08 m
    lines = []
    for n, mm in enumerate((8175, 6148, 8490, 8490, 3806), start=1):
        lines += [
            f"cs: t={n} a={n} execute init=0 refl=4 try={n} pair=4 budget=2400ms",
            f"cs: t={n} a={n} est ifft_mm={mm} phase_mm=9300 rtt_mm=4000 best_mm={mm} method=ifft",
            f"cs: t={n} a={n} result=SUCCESS dist_mm={mm} conf=900 phase=IDLE",
        ]
    noisy = chr(10).join(lines)
    attempts, _ = tool.parse(noisy)
    groups = tool.per_pair_scatter(attempts)
    assert groups == {(0, 4): [8175, 6148, 8490, 8490, 3806]}
    text = nl_join(tool.report("log", attempts, []))
    assert "pair 0-4: n=5 min=3806 max=8490 mean=7022 sd=1830 mm" in text


# The cable bound: every sensor hangs on a cable of 5 m from MAIN (node 0), so MAIN-to-sensor cannot exceed 5 m and sensor-to-
# sensor 10 m. Estimates of the bench logs 2026-11-02/03: 0->1 read 6.3..10.1 m with all three methods, 0->3 IFFT inside.
BOUND_LOG = """\
cs: t=1 a=1 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=2 a=1 est ifft_mm=7322 phase_mm=9378 rtt_mm=10106 best_mm=7322 method=ifft tones=70/75 gap=2 lead=2 tail=0
cs: t=3 a=1 result=SUCCESS dist_mm=7322 conf=900 phase=IDLE
cs: t=4 a=2 execute init=0 refl=3 try=1 pair=2 budget=2400ms
cs: t=5 a=2 est ifft_mm=3900 phase_mm=7315 rtt_mm=5908 best_mm=3900 method=ifft tones=60/75 gap=9
cs: t=6 a=2 result=SUCCESS dist_mm=3900 conf=900 phase=IDLE
cs: t=7 a=3 execute init=3 refl=4 try=1 pair=0 budget=2400ms
cs: t=8 a=3 est ifft_mm=3565 phase_mm=3919 rtt_mm=1038 best_mm=3565 method=ifft tones=72/75 gap=1
cs: t=9 a=3 result=SUCCESS dist_mm=3565 conf=900 phase=IDLE
cs: t=10 a=4 execute init=0 refl=4 try=1 pair=4 budget=2400ms
cs: t=11 a=4 est ifft_mm=10348 phase_mm=n/a rtt_mm=3465 best_mm=10348 method=ifft tones=40/75 gap=14 lead=14 tail=0
cs: t=12 a=4 result=SUCCESS dist_mm=10348 conf=900 phase=IDLE
"""


def test_the_cable_bound_is_one_cable_with_main_and_two_between_sensors():
    assert tool.pair_bound_mm((0, 1), 5000) == 5000
    assert tool.pair_bound_mm((1, 0), 5000) == 5000
    assert tool.pair_bound_mm((3, 4), 5000) == 10000


def test_estimates_outside_the_cable_bound_are_counted_per_method():
    attempts, links = tool.parse(BOUND_LOG)
    lines = tool.bound_check(attempts, 5000)
    text = chr(10).join(lines)
    assert "pair 0-1 bound 5000 mm: ifft in=0/1 median=7322; phase in=0/1 median=9378; rtt in=0/1 median=10106" in text
    assert "pair 0-3 bound 5000 mm: ifft in=1/1 median=3900; phase in=0/1 median=7315; rtt in=0/1 median=5908" in text
    assert "pair 3-4 bound 10000 mm: ifft in=1/1 median=3565; phase in=1/1 median=3919; rtt in=1/1 median=1038" in text
    assert "pair 0-4 bound 5000 mm: ifft in=0/1 median=10348; rtt in=1/1 median=3465" in text.replace("phase in=0/0 ", "")
    report = chr(10).join(tool.report("log", attempts, links, None, 5000))
    assert "estimates inside the cable bound (cable 5000 mm from MAIN)" in report
    assert "estimates inside the cable bound" not in chr(10).join(tool.report("log", attempts, links))   # off without --cable-mm


def test_the_holes_of_the_frequency_set_are_tabulated_against_the_cable_bound():
    attempts, _ = tool.parse(BOUND_LOG)
    assert attempts[1].tones == (70, 75, 2) and attempts[4].tones == (40, 75, 14)
    table = chr(10).join(tool.gap_table(attempts, 5000))
    # holes 1-2: attempts 1 (70 tones, ifft out of the bound) and 3 (72 tones, in); holes 6+: attempts 2 (60, in) and 4 (40, out)
    assert "hole 1-2  n=2   usable tones median=72  ifft inside the cable bound 1/2" in table
    assert "hole 6+   n=2   usable tones median=60  ifft inside the cable bound 1/2" in table
    # the hole of attempt 4 (14 slots) sits at the start of the array, like a channel 1 access point would make it
    assert "holes of 10+ slots at the start of the array: 1/2, at the end: 0/2" in table
    assert attempts[4].edges == (14, 0) and attempts[2].edges is None
    assert "hole 0 " not in table
    without_bound = chr(10).join(tool.gap_table(attempts, 0))
    assert "inside the cable bound" not in without_bound and "hole 1-2" in without_bound
    old, _ = tool.parse(ACCURACY_LOG)                       # old firmware: no tones, no table
    assert tool.gap_table(old, 5000) == []


SHARP_LOG = """cs: t=1 a=1 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=2 a=1 est ifft_mm=3100 phase_mm=3300 rtt_mm=2900 best_mm=3100 method=ifft tones=70/75 gap=2 lead=2 tail=0 pf=14.2
cs: t=3 a=1 result=SUCCESS dist_mm=3100 conf=900 phase=IDLE
cs: t=4 a=2 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=5 a=2 est ifft_mm=8800 phase_mm=3300 rtt_mm=2900 best_mm=8800 method=ifft tones=70/75 gap=2 lead=2 tail=0 pf=3.5
cs: t=6 a=2 result=SUCCESS dist_mm=8800 conf=900 phase=IDLE
cs: t=7 a=3 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=8 a=3 est ifft_mm=3000 phase_mm=3300 rtt_mm=2900 best_mm=3000 method=ifft tones=70/75 gap=2 lead=2 tail=0 pf=10.0
cs: t=9 a=3 result=SUCCESS dist_mm=3000 conf=900 phase=IDLE
cs: t=10 a=4 execute init=0 refl=1 try=1 pair=0 budget=2400ms
cs: t=11 a=4 est ifft_mm=3050 phase_mm=3300 rtt_mm=2900 best_mm=3050 method=ifft tones=70/75 gap=2 lead=2 tail=0
cs: t=12 a=4 result=SUCCESS dist_mm=3050 conf=900 phase=IDLE
"""


def test_the_peak_sharpness_is_parsed_and_old_logs_without_it_still_parse():
    attempts, _ = tool.parse(SHARP_LOG)
    assert [attempts[i].peak_floor for i in (1, 2, 3, 4)] == [14.2, 3.5, 10.0, None]
    assert attempts[1].estimates["ifft"] == 3100 and attempts[1].tones == (70, 75, 2)       # the other fields are unaffected


WHY_LOG = """cs: t=1 a=1 execute init=0 refl=4 try=1 pair=4 budget=2400ms
cs: t=2 a=1 est ifft_mm=5007 phase_mm=9336 rtt_mm=n/a best_mm=5007 method=ifft tones=63/72 gap=7 pf=28.1 stock_mm=1090 why=sharp_peak
cs: t=3 a=1 result=SUCCESS dist_mm=5007 conf=600 phase=IDLE
cs: t=4 a=2 execute init=0 refl=4 try=2 pair=4 budget=2400ms
cs: t=5 a=2 est ifft_mm=4954 phase_mm=9385 rtt_mm=n/a best_mm=4954 method=ifft tones=65/72 gap=3 pf=6.2 why=rtt_missing
cs: t=6 a=2 result=SUCCESS dist_mm=4954 conf=400 phase=IDLE
"""


def test_the_reason_and_the_stock_ifft_are_parsed_and_counted():
    attempts, links = tool.parse(WHY_LOG)
    assert (attempts[1].why, attempts[1].stock_ifft, attempts[1].peak_floor) == ("sharp_peak", 1090, 28.1)
    assert (attempts[2].why, attempts[2].stock_ifft) == ("rtt_missing", None)
    report = chr(10).join(tool.report("log", attempts, links))
    assert "confidence decided by (why=): rtt_missing=1, sharp_peak=1" in report
    assert "gap fill changed the library's IFFT in 1 attempts" in report
    assert "confidence decided by" not in chr(10).join(tool.report("log", tool.parse(BOUND_LOG)[0], []))     # old logs: nothing to say


def test_the_sharpness_table_compares_sharp_and_other_attempts_against_the_cable_bound():
    attempts, links = tool.parse(SHARP_LOG)
    text = chr(10).join(tool.sharpness_table(attempts, 5000))
    assert "peak/floor sharp (>10) n=1    median ratio=14.2  ifft inside the cable bound 1/1" in text
    assert "peak/floor not sharp  n=2    median ratio=10.0  ifft inside the cable bound 1/2" in text
    assert "attempts with a sharp peak: 1/3" in text                                            # exactly 10.0 is not sharp
    assert "inside the cable bound" not in chr(10).join(tool.sharpness_table(attempts, 0))     # off without --cable-mm
    assert tool.sharpness_table(tool.parse(BOUND_LOG)[0], 5000) == []                           # old logs: nothing to say
    report = chr(10).join(tool.report("log", attempts, links, None, 5000))
    assert "IFFT peak sharpness" in report
    assert "IFFT peak sharpness" not in chr(10).join(tool.report("log", tool.parse(BOUND_LOG)[0], []))
