#!/usr/bin/env python3
"""Grade ecnr_eval CSVs against a GB/T 45314-2025 eCall pre-compliance gate.

This is a software release gate for the ECNR chain. It maps the emergency-call
audio clauses that can be measured from desk fixtures onto objective proxies:

  * 5.1 Trtd < 210 ms / implementation delay <= 110 ms: frame latency proxy.
  * 5.5 TCL >= 46 dB: echo-only true ERLE proxy.
  * 5.5 initial convergence: table-7 style ERLE-at-time proxy.
  * 5.5.3 time-varying echo path: degradation <= 6 dB proxy.
  * 5.7 double-talk: near-end speech is not over-attenuated.
  * 5.8.1 B2 no-speech noise: transmitted noise level variation < 10 dB.

Lab-only measurements such as HATS SLR/RLR, POI levels, P.863 MOS-LQO, and
formal double-talk class 2b still require the standard test setup.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


INITIAL_CONVERGENCE = [
    ("erle_initial_0ms_db", 5.0),
    ("erle_initial_200ms_db", 5.0),
    ("erle_initial_1000ms_db", 20.0),
    ("erle_initial_1200ms_db", 40.0),
    ("erle_initial_1500ms_db", 40.0),
    ("erle_initial_5000ms_db", 40.0),
]


def parse_float(row: dict[str, str], key: str) -> float | None:
    raw = row.get(key, "")
    if raw == "" or raw.lower() == "nan":
        return None
    try:
        value = float(raw)
    except ValueError:
        return None
    if math.isnan(value):
        return None
    return value


def fail_if_missing(row: dict[str, str], key: str, fails: list[str]) -> float | None:
    value = parse_float(row, key)
    if value is None:
        fails.append(f"{key}: missing")
    return value


def grade_row(row: dict[str, str]) -> tuple[list[str], list[str]]:
    cid = row.get("condition_id", "")
    fails: list[str] = []
    warns: list[str] = []

    frame_ms = fail_if_missing(row, "frame_duration_ms", fails)
    if frame_ms is not None and frame_ms > 110.0:
        fails.append(f"frame_duration_ms={frame_ms:.1f} > 110 ms implementation-delay proxy")

    rtf = parse_float(row, "rtf")
    if rtf is not None and rtf >= 1.0:
        fails.append(f"rtf={rtf:.3f} >= 1.0 realtime budget")

    # Noise-only B2 is not an echo condition, so ERLE/TCL checks do not apply.
    is_noise_only = "noise_only" in cid or "b2_noise" in cid

    if not is_noise_only:
        tcl = fail_if_missing(row, "erle_true_median_db", fails)
        if tcl is not None and tcl < 46.0:
            fails.append(f"erle_true_median_db={tcl:.2f} dB < 46 dB TCL proxy")

        for key, floor in INITIAL_CONVERGENCE:
            value = fail_if_missing(row, key, fails)
            if value is not None and value < floor:
                fails.append(f"{key}={value:.2f} dB < {floor:.1f} dB convergence floor")

    if "timevarying" in cid:
        variation = fail_if_missing(row, "erle_time_variation_db", fails)
        if variation is not None and variation > 6.0:
            fails.append(f"erle_time_variation_db={variation:.2f} dB > 6 dB path-change limit")

    if is_noise_only:
        noise_range = fail_if_missing(row, "noise_level_range_db", fails)
        if noise_range is not None and noise_range >= 10.0:
            fails.append(f"noise_level_range_db={noise_range:.2f} dB >= 10 dB B2 no-speech limit")

    if "doubletalk" in cid:
        delta = fail_if_missing(row, "near_end_level_delta_median_db", fails)
        corr = fail_if_missing(row, "near_end_correlation", fails)
        if delta is not None and delta < -12.0:
            fails.append(f"near_end_level_delta_median_db={delta:.2f} dB < -12 dB preservation floor")
        if corr is not None and corr < 0.60:
            fails.append(f"near_end_correlation={corr:.2f} < 0.60 preservation floor")

    if not fails:
        if not is_noise_only:
            steady = parse_float(row, "erle_steady_median_db")
            if steady is not None and steady < 55.0:
                warns.append(f"erle_steady_median_db={steady:.2f} dB < 55 dB headroom target")
        if "doubletalk" in cid:
            delta = parse_float(row, "near_end_level_delta_median_db")
            if delta is not None and delta < -6.0:
                warns.append(f"near_end_level_delta_median_db={delta:.2f} dB < -6 dB soft target")
    return fails, warns


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in-csv", required=True, type=Path,
                    help="CSV emitted by ecnr_eval --run")
    args = ap.parse_args()

    if not args.in_csv.exists():
        print(f"--in-csv does not exist: {args.in_csv}", file=sys.stderr)
        return 1

    any_fail = False
    any_warn = False
    rows = 0
    print(f"GB/T 45314-2025 eCall pre-compliance gate: {args.in_csv}")
    print("=" * 78)
    with args.in_csv.open(newline="") as fin:
        reader = csv.DictReader(fin)
        if reader.fieldnames is None:
            print("CSV has no header row", file=sys.stderr)
            return 1
        for row in reader:
            rows += 1
            cid = row.get("condition_id", "<unknown>")
            fails, warns = grade_row(row)
            if fails:
                any_fail = True
                print(f"\n[FAIL] {cid}")
                for item in fails:
                    print(f"  {item}")
            elif warns:
                any_warn = True
                print(f"\n[WARN] {cid}")
                for item in warns:
                    print(f"  {item}")
            else:
                print(f"\n[PASS] {cid}")

    print(f"\nSummary: {rows} condition(s) graded.")
    if rows == 0:
        # An empty CSV means the eval run died before writing any
        # condition (e.g. chain init failure) — that is a BLOCK, never a
        # vacuous GREEN.
        print("Overall: BLOCK - CSV contains no graded conditions "
              "(eval run failed?).")
        return 1
    if any_fail:
        print("Overall: BLOCK - do not release for GB/T 45314 eCall validation.")
        return 1
    if any_warn:
        print("Overall: WARN - pre-compliance floors met; headroom targets pending.")
        return 2
    print("Overall: GREEN - software pre-compliance gate met.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
