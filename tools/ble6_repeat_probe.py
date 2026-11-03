"""Ablate repeated high-quality PCT pairs on identical Wieme procedures."""
import json
import math
import numpy as np
import ble6_research_replay as replay
from ble6_fft_benchmark import group, summarize
from ble6_svt_benchmark import combined


def first_good_per_channel(case):
    raw = case["raw_steps"]
    if len(raw) % 11:
        raise ValueError("Incomplete paired PCT step")
    counts = [0]*75
    selected = bytearray()
    for pos in range(0, len(raw), 11):
        step = raw[pos:pos+11]
        channel = step[0] - 2
        if 0 <= channel < 75 and not (step[5] & 15 or step[10] & 15) and not counts[channel]:
            selected.extend(step)
            counts[channel] = 1
    output = dict(case, raw_steps=bytes(selected), counts=counts)
    iq = combined(output)
    output["iq"] = np.column_stack((iq.real, iq.imag)).ravel().tolist()
    return output


def main():
    cases = list(replay.wieme_cases())
    inputs = [item for case in cases for item in (case, first_good_per_channel(case))]
    print("Running original and first-good-only on", len(cases), "Wieme procedures", flush=True)
    results, source = replay.build_and_replay(inputs, replay.BUILD / "repeat-probe")
    groups = {}
    for label in sorted({group(c) for c in cases}):
        indices = [i for i,c in enumerate(cases) if group(c) == label]
        valid = [i for i in indices if all(math.isfinite(results[2*i+j][1]) for j in (0, 1))]
        groups[label] = {"total": len(indices), "unavailable_either": len(indices)-len(valid),
            "procedures_with_repeats": sum(any(n > 1 for n in cases[i]["counts"]) for i in valid),
            "original": summarize(results[2*i][1]-cases[i]["truth_m"] for i in valid),
            "first_good": summarize(results[2*i+1][1]-cases[i]["truth_m"] for i in valid)}
    output = replay.BUILD / "repeat-probe.json"
    output.write_text(json.dumps({"cases":len(cases), "groups":groups, "sdk_sha256":replay.sha256(source),
        "note":"offline removal of repeats within same recordings; not a different radio measurement or the authors' MUSIC"},
        indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", output, flush=True)


if __name__ == "__main__":
    main()
