"""Replay installed Nordic cs_de and project confidence over optional BLE CS research data.

Third-party data stays in nRF54L15/tests/ble6, ignored by Git. The executable
uses a host FFT shim: this is not a Zephyr/controller or physical RF test.
"""
from __future__ import annotations
import bisect
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "nRF54L15/tests/ble6"
BUILD = ROOT / ".test-build/ble6-research"
WIEME = DATA / "Bluetooth Channel Sounding IQ Dataset Across Diverse Scenarios"
OWL = DATA / "OWL Enabling Orientation-Diverse Wireless Localization Research - Dataset"
XU = DATA / "Bl-CS-Evaluation-Dataset/Bl-CS-Evaluation-Dataset-main/Bl-CS-Evaluation-Dataset-main"


def pct_payload(i, q, quality):
    if not (-2048 <= i <= 2047 and -2048 <= q <= 2047 and 0 <= quality <= 3):
        raise ValueError("PCT must contain signed 12-bit IQ and a 2-bit quality")
    return b"\0" + ((i & 4095) | ((q & 4095) << 12)).to_bytes(3, "little") + bytes([quality])


def wieme_raw_steps(line):
    raw = bytearray()
    for step in line.strip().split(","):
        lq, rq, channel, antenna, a, b, c, d = map(int, step.split(";"))
        if antenna == 0 and 2 <= channel <= 76:
            raw.extend(bytes([channel]) + pct_payload(a, b, lq) + pct_payload(c, d, rq))
    return bytes(raw)


def wieme_row(line):
    totals = [[0.0] * 4 for _ in range(75)]
    counts = [0] * 75
    for step in line.strip().split(","):
        fields = step.split(";")
        if len(fields) != 8:
            raise ValueError("Wieme tone requires 8 fields")
        local_q, remote_q, channel, antenna, *values = map(int, fields)
        if local_q == remote_q == 0 and 2 <= channel <= 76 and antenna == 0:
            index = channel - 2
            counts[index] += 1
            for j, value in enumerate(values):
                totals[index][j] += value
    iq = []
    for count, values in zip(counts, totals):
        if count:
            a, b, c, d = (value / count for value in values)
            iq.extend((a * c - b * d, a * d + b * c))
        else:
            iq.extend((0.0, 0.0))
    return iq, counts


def mask_channels(case, first, last):
    """Remove measured CS channels, preserving the same physical recording."""
    masked = dict(case, iq=list(case["iq"]), counts=list(case["counts"]))
    for channel in range(first, last + 1):
        if not 2 <= channel <= 76:
            raise ValueError("CS channel must be in 2..76")
        index = channel - 2
        masked["iq"][2 * index:2 * index + 2] = [0.0, 0.0]
        masked["counts"][index] = 0
    if "raw_steps" in case:
        raw = case["raw_steps"]
        masked["raw_steps"] = b"".join(raw[i:i+11] for i in range(0, len(raw), 11)
                                        if not first <= raw[i] <= last)
    return masked


def nearest_reference(times, distances, timestamp):
    if not times:
        return None
    index = bisect.bisect_left(times, timestamp)
    candidates = [i for i in (index - 1, index) if 0 <= i < len(times)]
    best = min(candidates, key=lambda i: (abs(times[i] - timestamp), i))
    return distances[best] if abs(times[best] - timestamp) <= 0.5 else None


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def wieme_cases():
    with (WIEME / "mapping.csv").open(newline="", encoding="utf-8-sig") as stream:
        mappings = {r["filename"].replace(chr(92), "/"): r for r in csv.DictReader(stream)}
    with (WIEME / "outdoor_mapping.csv").open(newline="", encoding="utf-8-sig") as stream:
        outdoor = {r["filename"].replace(chr(92), "/"): r for r in csv.DictReader(stream)}
    for path in sorted((WIEME / "data").rglob("*.csv")):
        rel = path.relative_to(WIEME / "data").as_posix()
        if rel in mappings:
            ref = mappings[rel]
            truth = math.sqrt(sum((float(ref["initiator_" + axis]) - float(ref["reflector_" + axis])) ** 2 for axis in "xyz")) / 1000.0
            reference = "motion_capture_3d"
        elif rel in outdoor:
            truth = float(outdoor[rel]["distance(mm)"]) / 1000.0
            reference = "outdoor_reference"
        else:
            raise ValueError(f"Missing Wieme reference for {rel}")
        with path.open(encoding="utf-8") as stream:
            for index, line in enumerate(stream):
                iq, counts = wieme_row(line)
                yield {"dataset": "Wieme", "file": rel, "row": index, "truth_m": truth,
                       "reference": reference, "iq": iq, "counts": counts, "rtt": math.nan,
                       "raw_steps": wieme_raw_steps(line)}


def owl_cases(path, stride=200, include_diagnostics=False):
    import pyarrow.parquet as pq
    fields = ["technology", "status", "iq", "local_iq", "remote_iq", "tqi", "true_dist", "rtt", "ifft", "angle", "setup", "run_idx", "env"]
    file = pq.ParquetFile(path)
    diagnostic_fields=('local_rpl','remote_rpl','local_tx','remote_tx','nak','crc','rx_to',
                       'tone_RSSI','remote_tone_RSSI','median_RSSI','de_quality')
    if include_diagnostics:
        fields.extend(name for name in diagnostic_fields if name in file.schema_arrow.names)
    offset = 0
    for batch in file.iter_batches(batch_size=2000, columns=fields):
        selected = [i for i in range(batch.num_rows) if (offset + i) % stride == 0]
        for local_index, row in zip(selected, batch.take(selected).to_pylist()):
            if row["technology"] != "ble-cs" or row["status"] != "success" or not row["iq"] or not row["tqi"]:
                continue
            if len(row["iq"]) != 75 or len(row["tqi"]) != 75:
                raise ValueError("OWL tone array is not 75 slots")
            if not row["local_iq"] or not row["remote_iq"] or len(row["local_iq"]) != 75 or len(row["remote_iq"]) != 75:
                raise ValueError("OWL local/remote IQ array is not 75 slots")
            iq, counts = [], []
            for value, local, remote, quality in zip(row["iq"], row["local_iq"], row["remote_iq"], row["tqi"]):
                good = quality == "HIGH" and value is not None and len(value) == 2 and all(v is not None and math.isfinite(v) for v in value)
                if good:
                    if not local or not remote or len(local) != 2 or len(remote) != 2 or any(v is None or not math.isfinite(v) for v in local + remote):
                        raise ValueError("OWL HIGH tone lacks usable local/remote IQ")
                    re = local[0] * remote[0] - local[1] * remote[1]
                    im = local[0] * remote[1] + local[1] * remote[0]
                    if not math.isclose(value[0], re, abs_tol=0.01) or not math.isclose(value[1], im, abs_tol=0.01):
                        raise ValueError("OWL combined IQ does not match local times remote")
                counts.append(int(good)); iq.extend(value if good else (0.0, 0.0))
            rtt = row["rtt"] if row["rtt"] is not None and math.isfinite(row["rtt"]) and row["rtt"] > 0 else math.nan
            case = {"dataset": "OWL", "file": path.name, "row": offset + local_index, "truth_m": float(row["true_dist"]),
                   "angle": row["angle"], "setup": row["setup"], "run_idx": row["run_idx"], "env": row["env"],
                   "recorded_ifft": row["ifft"], "iq": iq, "counts": counts, "rtt": rtt}
            if include_diagnostics:
                case['diagnostics']={name:row.get(name) for name in diagnostic_fields}
                case['diagnostics']['tqi']=row['tqi']
            yield case
        offset += batch.num_rows


def build_and_replay(cases, workdir=BUILD, nfft=512):
    """Return stock IFFT, production distance/phase, and plain confidence control.

    Raw PCT records validate both the stock product of means and production mean
    of paired products independently. Actual production confidence is in extra.bin.
    """
    if nfft not in (512, 1024, 2048):
        raise ValueError("Unsupported FFT size")
    sdk = Path(os.environ.get("BLE6_NCS_ROOT", "C:/ncs/v3.4.0/nrf"))
    source = sdk / "subsys/bluetooth/cs_de/cs_de.c"
    if not source.is_file():
        raise FileNotFoundError(f"NCS 3.4.0 cs_de.c not found: {source}; set BLE6_NCS_ROOT")
    workdir.mkdir(parents=True, exist_ok=True)
    for name in ("zephyr/bluetooth/hci_types.h", "zephyr/logging/log.h", "dsp/transform_functions.h",
                 "dsp/fast_math_functions.h", "dsp/statistics_functions.h", "arm_const_structs.h"):
        path = workdir / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text("#pragma once\n")
    header = workdir / "bluetooth/cs_de.h"; header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("""#pragma once
#include <stdint.h>
#define CS_DE_NUM_CHANNELS 75
#define CONFIG_BT_CS_DE_NFFT_SIZE 512
#define CONFIG_BT_CS_DE_LOG_LEVEL 0
#define LOG_MODULE_REGISTER(...)
typedef struct {float i_local[75],q_local[75],i_remote[75],q_remote[75];} cs_de_iq_tones_t;
typedef enum {CS_DE_TONE_QUALITY_OK,CS_DE_TONE_QUALITY_BAD} cs_de_tone_quality_t;
typedef struct {float ifft,phase_slope,rtt,best;} cs_de_dist_estimates_t;
typedef enum {CS_DE_QUALITY_OK,CS_DE_QUALITY_DO_NOT_USE} cs_de_quality_t;
typedef struct {int role;uint8_t n_ap;cs_de_iq_tones_t iq_tones[1];cs_de_tone_quality_t tone_quality[1];cs_de_dist_estimates_t distance_estimates[1];int32_t rtt_accumulated_half_ns;uint8_t rtt_count;} cs_de_report_t;
float cs_de_rtt(int32_t,uint8_t);float cs_de_phase_slope(float *);float cs_de_ifft(float *);
""".replace("#define CONFIG_BT_CS_DE_NFFT_SIZE 512", f"#define CONFIG_BT_CS_DE_NFFT_SIZE {nfft}"))
    (workdir / "ble6_sdk_cs_de.c").write_text(f'#include "{source.as_posix()}"\n')
    # Compile unchanged production aggregation and installed Zephyr PCT decoding.
    provenance = []
    for path, names in (
        (ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_2.inc", ("accumulate_mean", "collect_pbr_step")),
        (sdk.parent / "zephyr/subsys/bluetooth/host/cs.c", ("bt_le_cs_parse_pct",)),
    ):
        contents = path.read_text(encoding="utf-8")
        for name in names:
            point = contents.index(name + "(")
            start = contents.rfind("\n", 0, point) + 1
            brace = contents.index("{", point)
            depth, end = 1, brace + 1
            while depth:
                depth += (contents[end] == "{") - (contents[end] == "}")
                end += 1
            body = contents[start:end]
            (workdir / (name + ".inc")).write_text(body + "\n", encoding="utf-8")
            provenance.append({"function": name, "path": str(path), "sha256": hashlib.sha256(body.encode()).hexdigest()})
    (workdir / "collector-provenance.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    input_path, output_path = workdir / "input.bin", workdir / "output.bin"
    with input_path.open("wb") as stream:
        for case in cases:
            raw = case.get("raw_steps", b"")
            if len(raw) % 11:
                raise ValueError("Raw paired Mode 2 step must contain channel and two 5-byte payloads")
            stream.write(struct.pack("<H", len(raw) // 11))
            stream.write(raw)
            stream.write(struct.pack("<150f75Hf", *case["iq"], *case["counts"], case["rtt"]))
    manifest = {"version": 1, "tests": [{"name": "ble6_research_replay",
        "sources": ["tests/host/ble6_research_replay.c", "nRF54L15/src/nrf_cs_measurement_quality.c"],
        "include_dirs": [workdir.relative_to(ROOT).as_posix(), "nRF54L15/include"], "libraries": ["m"]}]}
    manifest_path = workdir / "manifest.json"; manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    env = os.environ.copy(); env["BLE6_REPLAY_INPUT"] = str(input_path); env["BLE6_REPLAY_OUTPUT"] = str(output_path)
    env["BLE6_REPLAY_EXTRA"] = str(workdir / "extra.bin")
    env["BLE6_REPLAY_PEAK"] = str(workdir / "peak.bin")
    run = subprocess.run([sys.executable, "tools/run_host_tests.py", "--manifest", str(manifest_path),
                    "--build-dir", str(workdir / "native")], cwd=ROOT, env=env, check=False,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    (workdir / "run.log").write_text(run.stdout, encoding="utf-8")
    if run.returncode:
        raise RuntimeError(f"Native replay failed; {workdir / 'run.log'}\n{run.stdout}")
    results = list(struct.iter_unpack("<4f", output_path.read_bytes()))
    if len(results) != len(cases):
        raise ValueError(f"Replay length mismatch: {len(results)} / {len(cases)}")
    return results, source


def confidence_extra(workdir=BUILD):
    """Per procedure of the last build_and_replay(workdir): (IFFT peak over median bin, plain confidence, confidence with the peak, reason code)."""
    return list(struct.iter_unpack("<4f", (workdir / "extra.bin").read_bytes()))


def ocm_cases():
    from array import array
    base = DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1"
    with (base / "procedures.csv").open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    ids = {(r["segment"], r["proc"]): i for i, r in enumerate(rows)}
    if len(ids) != len(rows):
        raise ValueError("OCMTEC duplicate procedure keys")
    sums = [array("d", [0.0]) * 300 for _ in rows]
    counts = [array("H", [0]) * 75 for _ in rows]
    raw_steps = [bytearray() for _ in rows]
    with (base / "tones.csv").open(newline="", encoding="utf-8") as stream:
        for tone in csv.DictReader(stream):
            index = ids[(tone["segment"], tone["proc"])]
            channel = int(tone["channel"]) - 2
            if not 0 <= channel < 75:
                continue
            values = [int(tone[key]) for key in ("i_init", "q_init", "i_refl", "q_refl")]
            raw_steps[index].extend(bytes([channel + 2]) +
                pct_payload(*values[:2], int(tone["tqi_init"])) +
                pct_payload(*values[2:], int(tone["tqi_refl"])))
            if tone["tqi_init"] != "0" or tone["tqi_refl"] != "0":
                continue
            counts[index][channel] += 1
            for j, key in enumerate(("i_init", "q_init", "i_refl", "q_refl")):
                sums[index][4 * channel + j] += float(tone[key])
    for row, total, count, raw in zip(rows, sums, counts, raw_steps):
        if sum(c > 0 for c in count) != int(row["n_slots_used"]):
            raise ValueError(f"OCMTEC tone mismatch {row['segment']}/{row['proc']}")
        iq = []
        for i, n in enumerate(count):
            if n:
                a, b, c, d = (total[4 * i + j] / n for j in range(4))
                iq.extend((a * c - b * d, a * d + b * c))
            else:
                iq.extend((0.0, 0.0))
        rtt = float(row["fw_rtt"]) if row["fw_rtt"] and float(row["fw_rtt"]) > 0 else math.nan
        yield {"dataset": "OCMTEC", "file": row["segment"], "row": int(row["proc"]), "truth_m": 1.0,
               "author_ifft": float(row["replay_ifft"]) if row["replay_ifft"] else math.nan,
               "author_filled": float(row["replay_ifft_gapfill"]) if row["replay_ifft_gapfill"] else math.nan,
               "iq": iq, "counts": count, "rtt": rtt, "raw_steps": bytes(raw)}


def xu_time(value):
    from datetime import datetime
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%m/%d/%Y %H:%M:%S.%f"):
        try:
            return datetime.strptime(value, fmt).timestamp()
        except ValueError:
            pass
    raise ValueError(f"Unsupported Xu timestamp: {value}")


def xu_statistics():
    output = []
    for path in sorted((XU / "RangingFilter").glob("*.csv")):
        ref = XU / "GT" / path.name
        if not ref.is_file():
            raise FileNotFoundError(f"Xu reference missing for {path.name}")
        with ref.open(newline="", encoding="utf-8-sig") as stream:
            reference = sorted((xu_time(row["Timestamp"]), float(row["Distance(m)"])) for row in csv.DictReader(stream)
                               if row["Distance(m)"] and math.isfinite(float(row["Distance(m)"])))
        times, distances = zip(*reference) if reference else ([], [])
        matched = missing = nan_count = 0
        errors = []
        with path.open(newline="", encoding="utf-8-sig") as stream:
            for row in csv.DictReader(stream):
                try:
                    observed = float(row["Distance (m)"])
                except (TypeError, ValueError):
                    nan_count += 1; continue
                if not math.isfinite(observed):
                    nan_count += 1; continue
                actual = nearest_reference(times, distances, xu_time(row["Timestamp"]))
                if actual is None:
                    missing += 1; continue
                matched += 1; errors.append(abs(observed - actual))
        output.append({"file": path.name, "matched": matched, "unmatched": missing, "nan": nan_count,
                       "mae_m": sum(errors) / matched if matched else None})
    return output


def percentile(values, q):
    ordered = sorted(values)
    if not ordered:
        return None
    at = (len(ordered) - 1) * q
    lo, hi = math.floor(at), math.ceil(at)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (at - lo)


def waves_steps(data):
    steps = []
    offset = 0
    while offset < len(data):
        if offset + 3 > len(data):
            raise ValueError("Incomplete Waves CS step header")
        mode, channel, size = data[offset:offset + 3]
        start = offset + 3; end = start + size
        if end > len(data) or mode > 3 or channel > 78:
            raise ValueError("Invalid Waves CS step")
        if mode == 2:
            if size < 5 or (size - 1) % 4:
                raise ValueError("Invalid Waves mode-2 length")
            pct = int.from_bytes(data[start + 1:start + 4], "little")
            a, b = pct & 0xfff, (pct >> 12) & 0xfff
            steps.append((channel, a - 4096 if a >= 2048 else a,
                          b - 4096 if b >= 2048 else b, data[start + 4] & 0xf))
        offset = end
    return steps


def waves_log(path):
    import re
    text = path.read_text(encoding="utf-8")
    output = {}
    for block in re.findall(r"CS Subevent result received:(.*?)I: CS Subevent end", text, re.S):
        counter = re.search(r"Procedure counter:\s*(\d+)", block)
        data = re.search(r"Raw step data:\s*([0-9a-fA-F\s]+)$", block)
        if not counter:
            raise ValueError(f"Incomplete Waves subevent in {path}")
        if not data:
            if re.search(r"Num steps reported:\s*0", block):
                continue
            raise ValueError(f"Missing Waves step data in {path}")
        counter = int(counter.group(1))
        if counter in output:
            raise ValueError(f"Duplicate Waves procedure {counter}")
        output[counter] = waves_steps(bytes.fromhex(data.group(1)))
    return output


def waves_cases():
    base = DATA / "BLE Channel Sounding phase-based ranging analysis tool/waves-main/waves-main/tests"
    initiator, reflector = waves_log(base / "ini.txt"), waves_log(base / "ref.txt")
    if not initiator or not reflector:
        raise ValueError("No Waves subevents available")
    for counter in sorted(set(initiator) & set(reflector)):
        local, remote = initiator[counter], reflector[counter]
        if len(local) != len(remote) or any(a[0] != b[0] for a, b in zip(local, remote)):
            raise ValueError(f"Waves channel sequence mismatch in {counter}")
        sums = [[0.0] * 4 for _ in range(75)]; counts = [0] * 75
        for a, b in zip(local, remote):
            channel = a[0] - 2
            if 0 <= channel < 75 and a[3] == b[3] == 0:
                counts[channel] += 1
                for i, value in enumerate((a[1], a[2], b[1], b[2])):
                    sums[channel][i] += value
        iq = []
        for count, values in zip(counts, sums):
            if count:
                a, b, c, d = (v / count for v in values)
                iq.extend((a*c - b*d, a*d + b*c))
            else:
                iq.extend((0.0, 0.0))
        yield {"dataset": "Waves", "file": "ini.txt+ref.txt", "row": counter,
               "truth_m": None, "iq": iq, "counts": counts, "rtt": math.nan}


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--owl-stride", type=int, default=200, help="deterministic row-index stride in each OWL file")
    parser.add_argument("--max-error-m", type=float, default=1.0, help="diagnostic threshold, not a product acceptance limit")
    parser.add_argument("--strict", action="store_true", help="exit nonzero for any large distance error, regardless of confidence")
    args = parser.parse_args()
    if args.owl_stride < 1 or args.max_error_m <= 0:
        parser.error("stride and diagnostic threshold must be positive")
    cases = list(ocm_cases()) + list(wieme_cases()) + list(waves_cases())
    try:
        import pyarrow  # noqa: F401
    except ImportError as exc:
        raise SystemExit("OWL needs pyarrow; install it in a local environment or add .test-build/ble6-deps to PYTHONPATH") from exc
    for path in (OWL / "owl-indoor.parquet", OWL / "owl-outdoor.parquet"):
        cases.extend(owl_cases(path, args.owl_stride))
    results, source = build_and_replay(cases)
    confidence_details = confidence_extra()
    if len(confidence_details) != len(results):
        raise ValueError("Confidence sidecar length does not match replay")
    grouped = {}
    orientation = {}
    findings = {}
    waves_results = []
    reference_errors = []
    for case, (raw, filled, phase, _plain_control), details in zip(cases, results, confidence_details):
        confidence = details[2]
        if case["dataset"] == "OCMTEC" and math.isfinite(raw) and math.isfinite(case["author_ifft"]):
            reference_errors.append(abs(raw - case["author_ifft"]))
        if sum(n > 0 for n in case["counts"]) < 15 or not math.isfinite(filled) or filled < 0:
            continue
        if case["truth_m"] is None:
            waves_results.append({"procedure": case["row"], "raw_m": raw if math.isfinite(raw) else None,
                                  "filled_m": filled, "phase_m": phase if math.isfinite(phase) else None})
            continue
        group = case["dataset"] + ":" + case["file"]
        bucket = grouped.setdefault(group, {"n": 0, "errors": [], "large_error": 0, "high_confidence_large_error": 0})
        error = abs(filled - case["truth_m"])
        bucket["n"] += 1; bucket["errors"].append(error)
        if error > args.max_error_m:
            bucket["large_error"] += 1
        if case["dataset"] == "OWL" and case["angle"] is not None and math.isfinite(case["angle"]):
            quadrant = int((case["angle"] % (2 * math.pi)) / (math.pi / 2))
            key = f"{case['env']}:{case['setup']}:run{case['run_idx']}:quadrant{quadrant}"
            orientation.setdefault(key, []).append(error)
        if confidence >= 500 and error > args.max_error_m:
            bucket["high_confidence_large_error"] += 1
            examples = findings.setdefault(case["dataset"], [])
            if len(examples) < 10:
                examples.append({k: case[k] for k in ("dataset", "file", "row", "truth_m") if k in case} |
                                {"distance_m": round(filled, 5), "confidence": int(confidence),
                                 "rtt_available": math.isfinite(case["rtt"]), "error_m": round(error, 5)})
    for group in grouped.values():
        errors = group.pop("errors"); group["mae_m"] = sum(errors) / len(errors)
        group["median_error_m"] = percentile(errors, 0.5); group["p95_error_m"] = percentile(errors, 0.95)
    orientation_summary = {key: {"n": len(errors), "mae_m": sum(errors) / len(errors),
                                  "p95_error_m": percentile(errors, 0.95)} for key, errors in orientation.items()}
    if not reference_errors or max(reference_errors) > 0.00001:
        raise ValueError(f"SDK replay does not match OCMTEC firmware baseline: max={max(reference_errors, default=None)}")
    sources = [ROOT / "nRF54L15/include/nrf_cs_iq_gap_fill.h", ROOT / "nRF54L15/src/nrf_cs_measurement_quality.c",
               ROOT / "tests/host/ble6_research_replay.c", Path(__file__),
               DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1/tones.csv",
               DATA / "cs_de_gaps_data_v1/cs_de_gaps_data_v1/procedures.csv", WIEME / "mapping.csv", WIEME / "outdoor_mapping.csv",
               OWL / "owl-indoor.parquet", OWL / "owl-outdoor.parquet", source]
    sources.extend(sorted((WIEME / "data").rglob("*.csv")))
    sources.extend(sorted((XU / "GT").glob("*.csv"))); sources.extend(sorted((XU / "RangingFilter").glob("*.csv")))
    sources.extend(sorted((DATA / "BLE Channel Sounding phase-based ranging analysis tool/waves-main/waves-main/tests").glob("*.txt")))
    report = {"diagnostic_threshold_m": args.max_error_m, "owl_stride": args.owl_stride, "replayed": len(cases),
              "confidence_policy": "production nrf_cs_measurement_confidence_with_peak (extra.bin)",
              "raw_pct_procedures": sum(bool(case.get("raw_steps")) for case in cases),
              "raw_pct_steps": sum(len(case.get("raw_steps", b"")) // 11 for case in cases),
              "collector_provenance": json.loads((BUILD / "collector-provenance.json").read_text(encoding="utf-8")),
              "ocm_baseline_max_difference_m": max(reference_errors), "groups": grouped, "examples": findings,
              "xu_android_vs_uwb": xu_statistics(), "owl_orientation": orientation_summary,
              "waves_without_reference": waves_results,
              "sha256": {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path): sha256(path) for path in sources},
              "limitations": ["Host FFT shim, not target CMSIS or RF", "OWL is sampled by deterministic row index",
                              "Xu Android confidence is not project confidence", "Wieme has no RTT in these records",
                              "Waves recordings lack verified reference distance"]}
    report_path = BUILD / "report.json"; report_path.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    count = sum(group["high_confidence_large_error"] for group in grouped.values())
    distance_failures = sum(group["large_error"] for group in grouped.values())
    print(f"REPORT {report_path}: replayed={len(cases)} large_distance_error={distance_failures} high_confidence_large_error={count} threshold={args.max_error_m}m")
    if args.strict and distance_failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
