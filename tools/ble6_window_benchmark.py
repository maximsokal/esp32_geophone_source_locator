"""Explore fixed-grid tapers before the project's existing IFFT gap fill.

This host-only comparison does not change production or implement a new gate.
"""
import json
import math
import os
import argparse
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import numpy as np
import ble6_research_replay as replay
from ble6_fft_benchmark import group, summarize
from ble6_svt_benchmark import combined

NAMES = ("hann", "hamming", "blackman")


def apply_window(iq, name):
    iq = np.asarray(iq, dtype=complex)
    windows = {"hann": np.hanning, "hamming": np.hamming, "blackman": np.blackman}
    if iq.shape != (75,) or name not in windows:
        raise ValueError("Expected 75 tones and a supported window")
    return iq * np.maximum(windows[name](75), 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--full", action="store_true", help="All OCMTEC/Wieme; OWL retains stride 200")
    args = parser.parse_args()
    cases = [c for i, c in enumerate(replay.ocm_cases()) if args.full or i % 12 == 0]
    cases += [c for i, c in enumerate(replay.wieme_cases()) if args.full or i % 30 == 0]
    for name in ("owl-indoor.parquet", "owl-outdoor.parquet"):
        cases.extend(c for i, c in enumerate(replay.owl_cases(replay.OWL / name, 200)) if args.full or i % 4 == 0)
    inputs = []
    for case in cases:
        iq = combined(case)
        inputs.append(case)
        for name in NAMES:
            tapered = apply_window(iq, name)
            inputs.append(dict(case, iq=np.column_stack((tapered.real, tapered.imag)).ravel().tolist(), raw_steps=b""))
    print(f"Running {len(cases)} procedures with three windows", flush=True)
    results, source = replay.build_and_replay(inputs, replay.BUILD / "window-benchmark")
    report = {"cases": len(cases), "sdk_source_sha256": replay.sha256(source),
              "note": "Window applied before gap fill; OWL stride 200; not holdout; OCMTEC nominal contains bias",
              "groups": {}}
    for label in sorted({group(c) for c in cases}):
        indices = [i for i,c in enumerate(cases) if group(c) == label]
        matched = [i for i in indices if all(math.isfinite(results[4*i+j][1]) for j in range(4))]
        report["groups"][label] = {"unavailable_any": len(indices)-len(matched),
            **{name: summarize(results[4*i+j][1]-cases[i]["truth_m"] for i in matched)
               for j,name in enumerate(("baseline", *NAMES))}}
    output = replay.BUILD / ("window-benchmark-full.json" if args.full else "window-benchmark.json")
    output.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", output, flush=True)


if __name__ == "__main__":
    main()
