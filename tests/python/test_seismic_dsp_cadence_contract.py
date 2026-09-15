from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "components" / "seismic_dsp" / "include" / "seismic_dsp.h"
DOC = REPO_ROOT / "docs" / "SEISMIC_DSP_CADENCE_CONTRACT.md"


def test_public_header_exposes_fixed_rate_cadence_requirement():
    text = HEADER.read_text(encoding="utf-8")
    assert "SEISMIC_DSP_FIXED_RATE_REQUIRES_CADENCE_VALID_INPUT 1u" in text
    assert "sample_rate_hz is nominal/configured metadata only" in text
    assert "MUST NOT silently process irregular timestamped" in text


def test_architecture_note_defines_the_three_reviewed_gap_policies():
    text = DOC.read_text(encoding="utf-8").lower()
    assert "cadence-valid contiguous windows" in text
    assert "reset state around every known cadence gap" in text
    assert "resample irregularly timed data" in text
    assert "collapsed_periods > 0" in text
    assert "cadence evidence missing or invalid" in text
