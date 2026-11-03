"""Compare NCS 3.4 FFT sizes on the same optional research records.

The host FFT implementation is a shim; no target CMSIS timing is measured.
"""
import json
import math
from pathlib import Path
import sys

import ble6_research_replay as replay


def summarize(errors):
    errors = sorted(abs(x) for x in errors)
    n = len(errors)
    return {"n": n, "mae_m": sum(errors) / n if n else None,
            "rmse_m": math.sqrt(sum(x*x for x in errors) / n) if n else None,
            "p95_m": replay.percentile(errors, .95), "max_m": max(errors, default=None),
            "over_1m": sum(x > 1 for x in errors), "over_2m": sum(x > 2 for x in errors)}


def group(case):
    if case["dataset"] != "Wieme":
        return case["dataset"]
    name = case["file"]
    if "/NLOS/" in f"/{name}":
        return "Wieme NLOS"
    if "/LOS/" in f"/{name}":
        return "Wieme LOS"
    return "Wieme outdoor"


def main():
    cases = list(replay.ocm_cases()) + list(replay.wieme_cases())
    for name in ("owl-indoor.parquet", "owl-outdoor.parquet"):
        cases.extend(replay.owl_cases(replay.OWL / name, 200))
    results = {}
    for size in (512, 1024, 2048):
        print(f"Running Nordic FFT {size} on {len(cases)} cases", flush=True)
        results[size], source = replay.build_and_replay(cases, replay.BUILD / f"fft-benchmark-{size}", nfft=size)
    groups = sorted({group(c) for c in cases})
    report = {"cases": len(cases), "sdk_source_sha256": replay.sha256(source),
              "note": "paired finite results; OCMTEC nominal 1m includes documented systematic bias; OWL stride 200", "groups": {}}
    for label in groups:
        indices = [i for i,c in enumerate(cases) if group(c) == label and c["truth_m"] is not None]
        values = {}
        for field, index in (("stock_ifft", 0), ("project_selected", 1), ("phase_slope", 2)):
            matched = [i for i in indices if all(math.isfinite(results[size][i][index]) for size in results)]
            values[field] = {str(size): summarize(results[size][i][index] - cases[i]["truth_m"] for i in matched)
                             for size in results}
            values[field]["not_finite_any_size"] = len(indices) - len(matched)
        values["rtt"] = summarize(cases[i]["rtt"] - cases[i]["truth_m"] for i in indices
                                  if math.isfinite(cases[i]["rtt"]) and cases[i]["rtt"] > 0)
        values["rtt_unavailable"] = len(indices) - values["rtt"]["n"]
        report["groups"][label] = values
    output = replay.BUILD / "fft-benchmark.json"
    output.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", output, flush=True)


if __name__ == "__main__":
    main()
