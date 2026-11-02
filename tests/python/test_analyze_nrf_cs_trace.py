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
