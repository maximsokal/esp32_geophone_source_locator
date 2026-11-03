"""Exploratory contiguous-subarray MUSIC, not a reproduction of cited papers.

Missing channels are never compressed: every covariance snapshot contains 12
actually observed consecutive 1 MHz tones. Model order is fixed for comparison.
"""
import json
import math
import os
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import numpy as np
import ble6_research_replay as replay
from ble6_fft_benchmark import group, summarize
from ble6_svt_benchmark import combined

LENGTH = 12
GRID = np.linspace(0, 299792458. / (2 * 1e6), 512, endpoint=False)
STEERING = np.exp(-4j*np.pi*1e6*np.outer(np.arange(LENGTH), GRID)/299792458.)


def estimate(iq, mask, order):
    iq = np.asarray(iq, dtype=complex)
    mask = np.asarray(mask, dtype=bool)
    if iq.shape != (75,) or mask.shape != (75,) or order not in (1, 2):
        raise ValueError("Expected 75 slots and model order 1 or 2")
    if not np.isfinite(iq[mask]).all():
        raise ValueError("Observed IQ must be finite")
    snapshots = [iq[i:i+LENGTH] for i in range(76-LENGTH) if mask[i:i+LENGTH].all()]
    if len(snapshots) <= order:
        return math.nan
    samples = np.array(snapshots)
    scale = np.max(np.abs(samples))
    if not scale:
        return math.nan
    samples /= scale
    cov = samples.T @ samples.conj() / len(samples)
    eigenvalues, vectors = np.linalg.eigh(cov)
    noise = vectors[:, :LENGTH-order]
    spectrum = np.sum(np.abs(noise.conj().T @ STEERING)**2, axis=0)
    return float(GRID[np.argmin(spectrum)])


def main():
    cases = [c for i,c in enumerate(replay.ocm_cases()) if i % 12 == 0]
    cases += [c for i,c in enumerate(replay.wieme_cases()) if i % 30 == 0]
    for name in ("owl-indoor.parquet", "owl-outdoor.parquet"):
        cases.extend(c for i,c in enumerate(replay.owl_cases(replay.OWL / name, 200)) if i % 4 == 0)
    print("Running baseline and MUSIC candidates on", len(cases), "procedures", flush=True)
    baseline, source = replay.build_and_replay(cases, replay.BUILD / "frequency-music-baseline")
    candidates = []
    for case in cases:
        iq = combined(case)
        mask = np.asarray(case["counts"]) > 0
        candidates.append((estimate(iq, mask, 1), estimate(iq, mask, 2)))
    groups = {}
    for label in sorted({group(c) for c in cases}):
        indices = [i for i,c in enumerate(cases) if group(c) == label]
        valid = [i for i in indices if math.isfinite(baseline[i][1]) and all(math.isfinite(x) for x in candidates[i])]
        groups[label] = {"total":len(indices), "unavailable_either":len(indices)-len(valid),
            "baseline":summarize(baseline[i][1]-cases[i]["truth_m"] for i in valid),
            "order1":summarize(candidates[i][0]-cases[i]["truth_m"] for i in valid),
            "order2":summarize(candidates[i][1]-cases[i]["truth_m"] for i in valid)}
    output = replay.BUILD / "frequency-music.json"
    output.write_text(json.dumps({"cases":len(cases),"groups":groups,"sdk_sha256":replay.sha256(source),
        "note":"exploratory 12 consecutive observed tones, fixed 1/2 paths, full unambiguous grid; not cited paper implementation"},
        indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", output, flush=True)


if __name__ == "__main__":
    main()
