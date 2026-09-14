from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_sustained_hil_runner_is_passive_and_waits_for_real_cache_eviction():
    runner = read("tools/run_recorder_sustained_hil.ps1")

    assert '[string]$LogPort = "COM6"' in runner
    assert '"-Action", "monitor"' in runner
    assert '"-Action", "flash"' not in runner
    assert "This runner never flashes firmware" in runner
    assert "cacheCapacityFrames" in runner
    assert "requiredPersistedFrames" in runner
    assert "SavedReferenceFrames" in runner
    assert "EvictionMarginFrames" in runner
    assert "samples_written" in runner
    assert "/api/seismic/session" in runner
    assert "analyze_recorder_sustained.py" in runner
    assert "RECORDER_SUSTAINED_HIL_RESULT" in runner


def test_integrity_contract_exercises_psram_to_internal_dma_to_sd_production_path():
    recorder = read("components/seismic_recorder/seismic_recorder.c")
    filesystem = read("components/seismic_recorder/seismic_recorder_fs.c")
    reader = read("components/seismic_recorder/seismic_recorder_reader.c")

    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in recorder
    assert "s_batch = heap_caps_malloc(" in recorder
    assert "s_cache_storage = heap_caps_malloc(" in recorder
    assert "PSRAM cache=%u frames" in recorder

    assert "DMA_ATTR static seismic_frame_t s_dma_staging" in filesystem
    assert "memcpy(\n            s_dma_staging," in filesystem
    assert "fwrite(\n            s_dma_staging," in filesystem
    assert "RECORDER_IO_TIMING op=fwrite" in filesystem

    assert 'seismic_recorder_reader_log_path(\n                "cache"' in reader
    assert 'seismic_recorder_reader_log_path(\n                "raw"' in reader
    assert "WAVEFORM_READ_PATH source=%s" in reader


def test_sustained_analyzer_requires_throughput_margin_eviction_and_raw_reader_evidence():
    analyzer = read("tools/analyze_recorder_sustained.py")

    assert "required_stream_bps" in analyzer
    assert "required_margin_bps" in analyzer
    assert "aggregate_write_bps" in analyzer
    assert "cache_capacity + frame_count" in analyzer
    assert 'match.group("source").lower() == "raw"' in analyzer
    assert "integrity mismatch" in analyzer
    assert "minimum_overlap_frames" in analyzer
    assert "RECORDER_SUSTAINED_ANALYSIS" in analyzer
