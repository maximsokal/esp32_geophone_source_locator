from pathlib import Path
import shutil
import subprocess

import pytest

tkinter = pytest.importorskip("tkinter", reason="Tcl interpreter is required for RRAM loader tests")
SCRIPT = Path(__file__).resolve().parents[2] / "nRF54L15/scripts/nrf54l_rram_commit.tcl"


def interpreter(*, ready_after: int = 0, load_error: bool = False):
    tcl = tkinter.Tcl()
    tcl.eval("""
        set calls {}
        set polls 0
        proc nrf54l-load {file} {
            global calls load_error
            lappend calls [list load $file]
            if {$load_error} { error "simulated load failure" }
        }
        proc mww {address value} {
            global calls
            lappend calls [list write $address $value]
        }
        proc mem2array {name width address count} {
            global calls polls ready_after
            if {$width != 32 || $count != 1} { error "unexpected register read width/count" }
            upvar 1 $name values
            lappend calls [list read $address]
            set values(0) [expr {$polls >= $ready_after}]
            incr polls
        }
        proc sleep {ms} { global calls; lappend calls [list sleep $ms] }
        proc echo {message} { global calls; lappend calls [list echo $message] }
    """)
    tcl.setvar("ready_after", ready_after)
    tcl.setvar("load_error", int(load_error))
    tcl.call("source", str(SCRIPT))
    return tcl


def test_loader_commits_waits_and_restores_read_only_before_success():
    tcl = interpreter(ready_after=2)
    tcl.call("nrf54l-load-committed", "firmware with spaces.hex")
    calls = tcl.eval("set calls")
    assert calls.startswith("{load {firmware with spaces.hex}} {write 0x5004b008 1}")
    assert calls.count("read 0x5004b400") == 3
    assert calls.count("sleep 1") == 2
    assert calls.endswith("{write 0x5004b500 0} {echo {NRF_RRAM_COMMIT: PASS}}")


def test_loader_times_out_without_success_and_restores_read_only():
    tcl = interpreter(ready_after=2000)
    with pytest.raises(tkinter.TclError, match="RRAMC READY timeout"):
        tcl.call("nrf54l-load-committed", "firmware.hex")
    calls = tcl.eval("set calls")
    assert calls.count("read 0x5004b400") == 1000
    assert calls.endswith("{write 0x5004b500 0}")
    assert "PASS" not in calls


def test_failed_load_is_not_committed_or_reported_as_success():
    tcl = interpreter(load_error=True)
    with pytest.raises(tkinter.TclError, match="simulated load failure"):
        tcl.call("nrf54l-load-committed", "firmware.hex")
    calls = tcl.eval("set calls")
    assert "0x5004b008" not in calls
    assert "read " not in calls
    assert calls.endswith("{write 0x5004b500 0}")
    assert "PASS" not in calls


def test_installed_openocd_jim_runs_helper_without_target_access(tmp_path: Path):
    openocd = shutil.which("openocd")
    if openocd is None:
        pytest.skip("OpenOCD is unavailable for the offline Jim compatibility check")
    harness = tmp_path / "offline-rram.tcl"
    harness.write_text("""
        if {[llength [info commands mem2array]] == 0} {
            echo "OFFLINE_COMMIT_TEST: native mem2array is missing"
            shutdown error
        }
        set calls {}
        proc nrf54l-load {file} { global calls; lappend calls load }
        proc mww {address value} { global calls; lappend calls [list write $address $value] }
        proc mem2array {name width address count} {
            upvar 1 $name values
            set values(0) 1
        }
        set status [catch {
    """ + f"source {{{SCRIPT.as_posix()}}}\n" + """
            nrf54l-load-committed "not-a-real-image.hex"
            if {$calls != {load {write 0x5004b008 1} {write 0x5004b500 0}}} {
                error "unexpected offline sequence: $calls"
            }
        } detail]
        if {$status != 0} {
            echo "OFFLINE_COMMIT_TEST: FAIL $detail"
            shutdown error
        }
        echo "OFFLINE_COMMIT_TEST: PASS"
        shutdown
    """, encoding="ascii")
    result = subprocess.run([openocd, "-f", str(harness)], capture_output=True, text=True,
                            check=False, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "OFFLINE_COMMIT_TEST: PASS" in result.stdout + result.stderr
