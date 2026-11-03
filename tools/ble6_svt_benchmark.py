"""Evaluate the exploratory SVT probe on identical real IQ records.

This is not a reproduction of IEEE 10.1109/LWC.2026.3729084.
"""
import json
import math
import os
from pathlib import Path

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import numpy as np
import ble6_research_replay as replay
from ble6_fft_benchmark import group, summarize
from ble6_svt_probe import complete


def combined(case):
    raw = case.get("raw_steps", b"")
    if not raw:
        iq = np.asarray(case["iq"], dtype=float).reshape(-1, 2)
        return iq[:, 0] + 1j*iq[:, 1]
    accum = np.zeros(75, dtype=complex)
    counts = np.zeros(75, dtype=int)
    for k in range(0, len(raw), 11):
        b = raw[k:k+11]
        channel = b[0] - 2
        if channel < 0 or channel >= 75 or b[5] & 15 or b[10] & 15:
            continue
        def pct(data):
            packed = int.from_bytes(data, "little")
            i, q = packed & 4095, (packed >> 12) & 4095
            return complex(i - 4096 if i & 2048 else i, q - 4096 if q & 2048 else q)
        accum[channel] += pct(b[2:5]) * pct(b[7:10])
        counts[channel] += 1
    assert np.array_equal(counts, np.asarray(case["counts"]))
    return np.divide(accum, counts, out=np.zeros_like(accum), where=counts > 0)


def main():
    cases = [c for i, c in enumerate(replay.ocm_cases()) if i % 12 == 0]
    cases += [c for i, c in enumerate(replay.wieme_cases()) if i % 30 == 0]
    for name in ("owl-indoor.parquet", "owl-outdoor.parquet"):
        cases.extend(c for i, c in enumerate(replay.owl_cases(replay.OWL / name, 200)) if i % 4 == 0)
    inputs = []
    for c in cases:
        iq = combined(c)
        observed = np.asarray(c["counts"]) > 0
        if observed.sum() < 4:
            raise ValueError("Input has insufficient observed tones")
        filled = complete(iq, observed)
        present = np.flatnonzero(observed)
        counts = observed.astype(int)
        counts[present[0]:present[-1]+1] = 1
        candidate = dict(c, iq=np.column_stack((filled.real, filled.imag)).ravel().tolist(),
                         counts=counts.tolist(), raw_steps=b"")
        inputs.extend((c, candidate))
    print("SVT candidate pairs", len(cases), flush=True)
    result, source = replay.build_and_replay(inputs, replay.BUILD / "svt-probe-benchmark")
    groups = {}
    for label in sorted({group(c) for c in cases}):
        indices = [i for i, c in enumerate(cases) if group(c) == label]
        valid = [i for i in indices if all(math.isfinite(result[2*i+j][1]) for j in (0, 1))]
        groups[label] = {"unavailable_either": len(indices)-len(valid),
            "baseline": summarize(result[2*i][1]-cases[i]["truth_m"] for i in valid),
            "svt_probe": summarize(result[2*i+1][1]-cases[i]["truth_m"] for i in valid)}
    output = replay.BUILD / "svt-probe-benchmark.json"
    output.write_text(json.dumps({"cases":len(cases), "groups":groups,
        "note":"exploratory SVT, not IEEE algorithm; OCMTEC bias; deterministic samples, no holdout",
        "sdk_source_sha256": replay.sha256(source)}, indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", output, flush=True)


if __name__ == "__main__":
    main()
