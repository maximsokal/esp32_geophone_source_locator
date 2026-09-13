from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = REPO_ROOT / "tools" / "analyze_web_status_priority.py"

_spec = importlib.util.spec_from_file_location("analyze_web_status_priority", ANALYZER_PATH)
assert _spec is not None and _spec.loader is not None
analyzer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = analyzer
_spec.loader.exec_module(analyzer)


RUNTIME_BASE = (
    "I RUNTIME: RUNTIME_TASK rank=3 name=web_status_cache core=0 "
    "priority_current=1 priority_base=1 inherited=0 runtime_delta_us=10 cpu=0.1 stack_free=1000\n"
)
RUNTIME_INHERITED = (
    "I RUNTIME: RUNTIME_TASK rank=2 name=web_status_cach core=0 "
    "priority_current=23 priority_base=1 inherited=1 runtime_delta_us=100 cpu=5.0 stack_free=900\n"
)
STORAGE_BASE = (
    "I STORAGE_MANAGER: STORAGE_PRIORITY stage=pre-release owner=web_status_cache "
    "priority_current=1 priority_base=1 inherited=0 elapsed_us=1000\n"
)
STORAGE_INHERITED = (
    "I STORAGE_MANAGER: STORAGE_PRIORITY stage=pre-release owner=web_status_cach "
    "priority_current=23 priority_base=1 inherited=1 elapsed_us=240000\n"
)


def test_no_evidence_is_blocked():
    result = analyzer.analyze("unrelated log\n")
    assert result.status == "BLOCKED"
    assert result.sample_count == 0
    assert result.violations == ()


def test_base_priority_runtime_sample_passes():
    result = analyzer.analyze(RUNTIME_BASE)
    assert result.status == "PASS"
    assert result.runtime_sample_count == 1
    assert result.storage_pre_release_sample_count == 0
    assert result.max_current_priority == 1


def test_historical_priority_23_inherited_sample_passes():
    result = analyzer.analyze(RUNTIME_INHERITED, require_inherited=True)
    assert result.status == "PASS"
    assert result.inherited_sample_count == 1
    assert result.max_current_priority == 23


def test_require_inherited_without_boost_is_blocked():
    result = analyzer.analyze(RUNTIME_BASE, require_inherited=True)
    assert result.status == "BLOCKED"
    assert "inherited web_status_cache priority sample" in result.missing


def test_storage_pre_release_is_counted_as_stronger_source():
    result = analyzer.analyze(STORAGE_INHERITED, require_storage_pre_release=True)
    assert result.status == "PASS"
    assert result.runtime_sample_count == 0
    assert result.storage_pre_release_sample_count == 1
    assert result.inherited_sample_count == 1


def test_require_storage_marker_without_it_is_blocked():
    result = analyzer.analyze(RUNTIME_INHERITED, require_storage_pre_release=True)
    assert result.status == "BLOCKED"
    assert "STORAGE_PRIORITY pre-release web_status_cache sample" in result.missing


def test_wrong_base_priority_fails():
    content = RUNTIME_BASE.replace("priority_current=1 priority_base=1", "priority_current=2 priority_base=2")
    result = analyzer.analyze(content)
    assert result.status == "FAIL"
    assert any("priority_base=2 expected=1" in item for item in result.violations)


def test_inherited_flag_must_match_current_vs_base():
    bad_high = RUNTIME_INHERITED.replace("inherited=1", "inherited=0")
    result = analyzer.analyze(bad_high)
    assert result.status == "FAIL"
    assert any("inconsistent with current=23 base=1" in item for item in result.violations)

    bad_equal = RUNTIME_BASE.replace("inherited=0", "inherited=1")
    result = analyzer.analyze(bad_equal)
    assert result.status == "FAIL"
    assert any("inconsistent with current=1 base=1" in item for item in result.violations)


def test_minimum_sample_count_blocks_short_capture():
    result = analyzer.analyze(RUNTIME_BASE + STORAGE_BASE, minimum_samples=3)
    assert result.status == "BLOCKED"
    assert result.sample_count == 2
    assert "web_status_cache priority samples (2/3)" in result.missing


def test_mixed_sources_report_counts_and_maximum():
    content = RUNTIME_BASE + RUNTIME_INHERITED + STORAGE_BASE + STORAGE_INHERITED
    result = analyzer.analyze(content)
    assert result.status == "PASS"
    assert result.sample_count == 4
    assert result.runtime_sample_count == 2
    assert result.storage_pre_release_sample_count == 2
    assert result.inherited_sample_count == 2
    assert result.max_current_priority == 23
