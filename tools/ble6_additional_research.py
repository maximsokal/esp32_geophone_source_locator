"""Evaluate optional maycondsn/Milan datasets without changing production algorithms.

Input directories remain gitignored. Missing downloads are never fabricated.
"""
from __future__ import annotations
import csv
import ast
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
from collections import defaultdict
import ble6_research_replay as replay

ROOT = replay.ROOT
DATA = replay.DATA
BUILD = ROOT / ".test-build/ble6-new-datasets"


def project(name, marker):
    matches = list((DATA / name).rglob(marker))
    if len(matches) != 1:
        raise ValueError(f"Expected one {marker} in {name}, found {len(matches)}")
    return matches[0].parent


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def rows(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        yield from csv.DictReader(line for line in stream if line.strip() and not line.startswith("CSV_DONE"))


def metrics(errors, unit="m"):
    values = [float(x) for x in errors]
    absolute = [abs(x) for x in values]
    n = len(values)
    return {"n": n, f"bias_{unit}": sum(values)/n if n else None,
            f"mae_{unit}": sum(absolute)/n if n else None,
            f"rmse_{unit}": math.sqrt(sum(x*x for x in values)/n) if n else None,
            f"p95_{unit}": replay.percentile(absolute, .95), f"max_{unit}": max(absolute, default=None),
            f"over_1{unit}": sum(x > 1 for x in absolute), f"over_2{unit}": sum(x > 2 for x in absolute)}


def may_file(path, issues=None):
    groups = {}
    quarantined = set()
    metadata_fields = ("rtt_acc_half_ns", "rtt_count", "quality", "ifft", "phase_slope", "rtt", "best")
    for row in rows(path):
        key = int(row["procedure"]), int(row["ap"])
        if key[1] != 0:
            raise ValueError("Only the project's one-antenna-path report is supported")
        channel = int(row["channel"])
        if not 0 <= channel < 75:
            if issues is None:
                raise ValueError("maycondsn channel is a 0..74 array slot, not a physical channel")
            issues.append({"file": path.name, "procedure": key[0], "ap": key[1],
                           "reason": "INVALID_SLOT", "slot": channel})
            quarantined.add(key)
            continue
        record = groups.setdefault(key, {"channels": {}, "metadata": tuple(row[f] for f in metadata_fields)})
        if record["metadata"] != tuple(row[f] for f in metadata_fields):
            raise ValueError(f"Changing procedure metadata in {key}")
        if channel in record["channels"]:
            raise ValueError(f"Duplicate channel {channel} in {key}")
        values = tuple(float(row[f]) for f in ("i_local", "q_local", "i_remote", "q_remote"))
        if not all(math.isfinite(x) for x in values):
            raise ValueError("Non-finite exported IQ")
        record["channels"][channel] = values
    for (procedure, ap), record in sorted(groups.items()):
        if (procedure, ap) in quarantined:
            continue
        tones = [[0.0]*75 for _ in range(4)]
        counts = [0]*75
        for channel, values in record["channels"].items():
            for i, value in enumerate(values):
                tones[i][channel] = value
            # The exporter emits a row only when m_n_iqs[ap][slot] > 0.
            # A reported zero is an observed (quantized) tone, not a missing slot.
            counts[channel] = 1
        acc, count, quality, *estimates = record["metadata"]
        acc, count = int(acc), int(count)
        if not -2**31 <= acc < 2**31 or not 0 <= count <= 255:
            raise ValueError("RTT accumulation/count outside SDK type range")
        yield {"file": path.name, "procedure": procedure, "ap": ap, "tones": tones, "counts": counts,
               "acc": acc, "count": count, "quality": int(quality), "recorded": [float(v) for v in estimates],
               "truth_m": float(path.stem.removesuffix("m"))}


def report_replay(cases):
    work = BUILD / "may-native"
    # Reuse the installed SDK headers/FFT shim and extracted-code provenance setup.
    replay.build_and_replay([], workdir=work)
    input_path, output_path = work / "reports.bin", work / "reports-out.bin"
    with input_path.open("wb") as stream:
        for case in cases:
            stream.write(struct.pack("<300f75HiBB", *(x for a in case["tones"] for x in a),
                *case["counts"], case["acc"], case["count"], int(sum(case["counts"]) >= 15)))
    manifest = {"version": 1, "tests": [{"name": "ble6_report_replay", "sources": [
        "tests/host/ble6_report_replay.c", "nRF54L15/src/nrf_cs_measurement_quality.c"],
        "include_dirs": [work.relative_to(ROOT).as_posix(), "nRF54L15/include"], "libraries": ["m"]}]}
    manifest_path = work / "reports-manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    env = os.environ.copy()
    env["BLE6_REPORT_INPUT"], env["BLE6_REPORT_OUTPUT"] = str(input_path), str(output_path)
    env["BLE6_REPORT_FIRMWARE"] = str(work / "firmware.bin")
    run = subprocess.run([sys.executable, "tools/run_host_tests.py", "--manifest", str(manifest_path),
        "--build-dir", str(work / "reports-native")], cwd=ROOT, env=env, capture_output=True,
        text=True, encoding="utf-8", errors="replace")
    (work / "reports-run.log").write_text(run.stdout + run.stderr, encoding="utf-8")
    if run.returncode:
        raise RuntimeError(f"SDK report replay failed: {work / 'reports-run.log'}")
    results = list(struct.iter_unpack("<10f", output_path.read_bytes()))
    if len(results) != len(cases):
        raise ValueError("Report replay length mismatch")
    firmware = replay.firmware_results(work)
    if len(firmware) != len(cases):
        raise ValueError("Report firmware sidecar length mismatch")
    return results


def may_analysis():
    base = project("Divergencia-do-BLE-CS-para-deteccao-de-ataques", "dump.py")
    files = sorted((base / "sessoes/20260623_dataset").glob("*.csv"))
    if len(files) != 10:
        raise ValueError(f"Expected 10 maycondsn CSV files, found {len(files)}")
    issues = []
    cases = [c for p in files for c in may_file(p, issues)]
    results = report_replay(cases)
    firmware = replay.firmware_results(BUILD / "may-native")
    # Only reviewed constants and pure numerical functions; never execute the
    # author's capture/main routines, pandas imports, calibration or serial I/O.
    import numpy as np
    author_path = base / "cs_de.py"
    parsed = ast.parse(author_path.read_text(encoding="utf-8"))
    pure = [node for node in parsed.body if isinstance(node, ast.Assign)
            or isinstance(node, ast.FunctionDef) and node.name != "main"]
    author = {"math": math, "np": np}
    exec(compile(ast.Module(body=pure, type_ignores=[]), str(author_path), "exec"), author)
    author_differences = {name: [] for name in ("ifft", "phase", "rtt_clamped")}
    for case, result in zip(cases, results):
        iq = author["cs_de_combined_iq_calculate"](*case["tones"])
        values = [author["cs_de_ifft"](iq), author["cs_de_phase_slope"](iq),
                  author["cs_de_rtt"](case["acc"], case["count"])]
        if math.isfinite(values[2]): values[2] = max(0.0, values[2])
        for i, name in enumerate(author_differences):
            if math.isfinite(values[i]) and math.isfinite(result[i]):
                author_differences[name].append(abs(values[i]-result[i]))
    grouped = defaultdict(list)
    for case, result, fw in zip(cases, results, firmware, strict=True):
        grouped[case["file"]].append((case, result, fw))
    output = {}
    for name, group in grouped.items():
        methods = {}
        for label, column in (("stock_ifft", 0), ("phase", 1), ("rtt_raw", 2), ("project_best", 4)):
            usable = [(c, r[column]) for c, r, _ in group if math.isfinite(r[column]) and r[column] >= 0]
            methods[label] = metrics(value-c["truth_m"] for c, value in usable)
            methods[label]["unavailable"] = len(group)-len(usable)
            methods[label]["zero"] = sum(value == 0 for _, value in usable)
        delta = {}
        for i, label in enumerate(("ifft", "phase", "rtt", "best")):
            differences = [abs(c["recorded"][i]-r[i]) for c, r, _ in group
                if math.isfinite(c["recorded"][i]) and math.isfinite(r[i])]
            delta[label] = {"n": len(differences), "max_difference_m": max(differences, default=None),
                            "above_1mm": sum(d > .001 for d in differences),
                            "finite_status_mismatch": sum(math.isfinite(c["recorded"][i]) != math.isfinite(r[i]) for c, r, _ in group)}
        accepted = [(c, fw) for c, _, fw in group if fw[4] and fw[1] >= 500]
        output[name] = {"procedures": len(group), "nominal_m": group[0][0]["truth_m"],
            "metrics": methods, "published_equivalence": delta,
            "nonpositive_rtt_acc": sum(c["count"] > 0 and c["acc"] <= 0 for c, _, _ in group),
            "plain_accepted": sum(r[7] >= 500 for _, r, _ in group),
            "legacy_peak_accepted": sum(r[8] >= 500 for _, r, _ in group),
            "production_unavailable_or_outlier": sum(not fw[4] for _, _, fw in group),
            "production_accepted": len(accepted),
            "production_accepted_error_over_1m": sum(abs(fw[0]/1000-c["truth_m"]) > 1 for c, fw in accepted)}
    return {"files": len(files), "procedures": len(cases), "tones": sum(sum(1 for _ in rows(p)) for p in files),
            "results": output, "quarantined": issues,
            "author_python_vs_sdk": {k: metrics(v) for k, v in author_differences.items()},
            "limitations": ["nominal file-name distances", "already aggregated IQ, no original repeated PCT pairs",
                "tone availability from exported row presence; individual TQI not exported", "all 75 channels assumed planned",
                "host FFT shim and post-estimator policy, no RAS/controller execution"],
            "confidence_policy": "production counts use firmware sidecar: with_methods and 1800 mm PBR calibration",
            "raw_control_policy": "original 10-float replay metrics, including project_best, are raw pre-calibration controls"}, files+[author_path]


def circle_position(a, b, r1, r2):
    if r1 is None or r2 is None or r1 < 0 or r2 < 0:
        return None
    baseline = math.dist(a, b)
    if baseline <= 0 or r1+r2 < baseline-1e-4 or abs(r1-r2)>baseline+1e-4:
        return None
    along = (r1*r1-r2*r2+baseline*baseline)/(2*baseline)
    height2 = r1*r1-along*along
    if height2 < -1e-4:
        return None
    ux, uy = (b[0]-a[0])/baseline, (b[1]-a[1])/baseline
    height = math.sqrt(max(0, height2))
    return a[0]+along*ux-height*uy, a[1]+along*uy+height*ux


def milan_ranges(base):
    grouped = defaultdict(list)
    files = sorted((base / "data/experiment1").glob("*.csv")) + sorted((base / "data/experiment2").rglob("*.csv"))
    for path in files:
        category = "experiment1/Outdoor_LOS" if path.parent.name == "experiment1" else "experiment2/"+path.parent.name
        grouped[category].extend(rows(path))
    result = {}
    for category, records in grouped.items():
        measurements = [r for r in records if number(r.get("true_distance_m")) is not None]
        methods = {}
        for method in ("ifft", "phase_slope", "rtt"):
            pairs = [(number(r.get(method+"_m")), number(r.get("true_distance_m"))) for r in measurements]
            valid = [(v, t) for v, t in pairs if v is not None and t is not None and v >= 0]
            methods[method] = metrics(v-t for v, t in valid)
            methods[method]["zero"] = sum(v == 0 for v, _ in valid)
            methods[method]["unavailable"] = len(measurements)-len(valid)
        result[category] = {"rows": len(measurements), "nonmeasurement_event_rows": len(records)-len(measurements), "files": sum(1 for p in files if
            ("experiment1/Outdoor_LOS" if p.parent.name == "experiment1" else "experiment2/"+p.parent.name) == category), "metrics": methods}
    return result, files


def milan_static(base):
    files = sorted((base / "data/experiment3").glob("*.csv"))
    errors, reconstructed, equality = [], [], []
    ranges = {m: [] for m in ("ifft", "phase_slope", "rtt")}
    invalid = inconsistent = total = 0
    per_file = {}
    for path in files:
        local_errors = []
        for row in rows(path):
            total += 1
            gt = tuple(float(row["gt_"+axis+"_m"]) for axis in "xy")
            a = tuple(float(row["r1_"+axis+"_m"]) for axis in "xy")
            b = tuple(float(row["r2_"+axis+"_m"]) for axis in "xy")
            for method in ranges:
                for node, anchor in (("r1", a), ("r2", b)):
                    distance = number(row.get(node+"_"+method+"_m"))
                    if distance is not None and distance >= 0:
                        ranges[method].append(distance-math.dist(anchor, gt))
            r1, r2 = number(row["r1_pos_distance_m"]), number(row["r2_pos_distance_m"])
            point = circle_position(a, b, r1, r2)
            if point is None:
                inconsistent += 1
            x, y = number(row["pos_x_m"]), number(row["pos_y_m"])
            if row["pos_valid"] != "1" or x is None or y is None:
                invalid += 1
                continue
            error = math.dist((x, y), gt)
            errors.append(error); local_errors.append(error)
            if point is not None:
                reconstructed.append(math.dist(point, gt))
                equality.append(math.dist(point, (x, y)))
        per_file[path.name] = metrics(local_errors)
    return {"files": len(files), "rows": total, "position_error": metrics(errors), "position_invalid": invalid,
            "inconsistent_circles": inconsistent, "reconstructed_position_error": metrics(reconstructed),
            "position_reproduction_difference": metrics(equality), "range_errors": {k: metrics(v) for k, v in ranges.items()},
            "per_file": per_file}, files


def milan_dynamic(base):
    summaries, files = [], []
    for metadata_path in sorted((base / "data/experiment4").rglob("*_metadata.json")):
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        tag = metadata_path.name.removesuffix("_metadata.json")
        folder = metadata_path.parent
        setup_path = folder/(tag+"_setup.json")
        setup = json.loads(setup_path.read_text(encoding="utf-8")) if setup_path.exists() else metadata["setup"]
        channel_path, raw_path = folder/(tag+"_channel_sounding.csv"), folder/(tag+"_raw_serial.csv")
        files.extend(p for p in (metadata_path, setup_path, channel_path, raw_path, folder/(tag+"_robot_log.csv")) if p.exists())
        if not channel_path.exists():
            summaries.append({"run": tag, "status": "MISSING_CHANNEL_CSV"})
            continue
        records = list(rows(channel_path))
        mismatch = missing_time = outside = usable = 0
        reported_anchors = {"1": set(), "2": set()}
        lags, proxy_errors, latency = [], {m: [] for m in ("ifft", "phase_slope", "rtt")}, []
        start, stop = metadata["robot_start_monotonic_ns"], metadata["robot_stop_monotonic_ns"]
        if stop <= start:
            raise ValueError(f"Nonpositive drive duration in {tag}")
        for row in records:
            anchor = setup["r"+row["reflector_id"]]
            measured_anchor = float(row["anchor_x_m"]), float(row["anchor_y_m"])
            reported_anchors[row["reflector_id"]].add(measured_anchor)
            bad_anchor = math.dist(anchor, measured_anchor) > .0001
            mismatch += int(bad_anchor)
            timestamp = number(row.get("pi_estimated_measurement_monotonic_ns"))
            delay = number(row.get("procedure_latency_ms"))
            if delay is not None: latency.append(delay/1000)
            if timestamp is None:
                missing_time += 1; continue
            receive = number(row.get("pi_receive_monotonic_ns"))
            if receive is not None: lags.append((receive-timestamp)/1e9)
            if not start <= timestamp <= stop:
                outside += 1; continue
            if bad_anchor or row["cs_valid"] != "1": continue
            fraction = (timestamp-start)/(stop-start)
            point = [setup["path_start"][i]+fraction*(setup["path_end"][i]-setup["path_start"][i]) for i in (0,1)]
            reference = math.dist(point, anchor)
            usable += 1
            for method in proxy_errors:
                distance = number(row.get(method+"_m"))
                if distance is not None and distance >= 0: proxy_errors[method].append(distance-reference)
        headers = []
        if raw_path.exists():
            headers = [r["raw_line"] for r in rows(raw_path) if "R2_POS=" in r.get("raw_line", "")]
        consistent_rows = usable
        if mismatch:
            usable = 0
            proxy_errors = {m: [] for m in proxy_errors}
        summaries.append({"run": tag, "status": "ANCHOR_CONFLICT" if mismatch else "CONSISTENT_RECORDED_ANCHORS",
            "rows": len(records), "anchor_mismatch_rows": mismatch, "missing_measurement_time": missing_time,
            "outside_drive_window": outside, "proxy_usable_rows": usable,
            "consistent_rows_before_run_quarantine": consistent_rows,
            "receive_lag_s": metrics(lags, unit="s"), "procedure_duration_s": metrics(latency, unit="s"),
            "linear_motion_proxy_errors": {k: metrics(v) for k, v in proxy_errors.items()},
            "setup_r2": setup["r2"], "reported_anchors": {k: sorted(v) for k, v in reported_anchors.items()},
            "initial_r2_headers": headers[:1]})
    return {"runs": len(summaries), "conflicting_runs": sum(x["status"] == "ANCHOR_CONFLICT" for x in summaries),
            "results": summaries, "limitations": ["linear motion is a model, not measured position ground truth",
                "entire conflicting runs excluded from proxy error", "initial boot header not used as current anchor geometry"]}, files


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    may, sources = may_analysis()
    milan = project("Bluetooth_channel_sounding_localization", "analyse_experiment_3.py").parent
    distance, paths = milan_ranges(milan); sources.extend(paths)
    static, paths = milan_static(milan); sources.extend(paths)
    dynamic, paths = milan_dynamic(milan); sources.extend(paths)
    report = {"maycondsn": may, "milan_ranges": distance, "milan_static": static, "milan_dynamic": dynamic,
        "sha256": {p.relative_to(ROOT).as_posix(): replay.sha256(p) for p in sorted(set(sources))}}
    target = BUILD / "report.json"
    target.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    print(f"REPORT {target}: may_procedures={may['procedures']} milan_static_rows={static['rows']} dynamic_runs={dynamic['runs']}")


if __name__ == "__main__":
    main()
