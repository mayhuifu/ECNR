#!/usr/bin/env python3
"""Check an augmented eval CSV against the Phase-1 acceptance bar (ADR-0012).

Reads the CSV produced by `reference/score_mos.py` (which itself consumes
`ecnr_eval --run` output) and grades each condition row against the
per-metric floors + soft targets locked in ADR-0012 §2. Per-condition
pass/fail; exits non-zero on ANY floor miss on ANY condition.

The targets are intentionally OPINION-DENSE — they reflect the ADR-0012
§2 numbers, which will be re-derived against Phase 2 cabin recordings.
Bump this file when the ADR's numbers change; this script is the
single point of enforcement so the gate and the ADR stay in sync.

Usage:
  python3 reference/check_acceptance_bar.py \\
    --in-csv /tmp/eval/results_with_mos.csv

Exit codes:
  0  all conditions pass all floors (and meet all soft targets)
  1  at least one condition misses a floor (block)
  2  at least one condition meets all floors but misses a soft target (warn)
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


# ADR-0012 §2.1 v2 (2026-05-31) — measured-baseline-informed targets.
# Authoritative source: docs/adr/0012-phase-1-acceptance-bar.md §2.1.
# Phase 1 cut; supersede via ADR-0013 once Phase 2 cabin data exists.
# Note: this gate is per-condition (one row = one fixture); it does NOT
# implement the per-scenario applicability matrix from ADR-0012 §3.1 —
# that lives in run_aec_challenge.py where scenario context is available.
BAR = [
    # column,                target,  floor,  units
    ("erle_true_median_db",  20.0,    12.0,   "dB"),
    ("dnsmos_sig",            3.3,     3.0,    "MOS"),  # v2: target 3.5 → 3.3
    ("dnsmos_bak",            3.5,     3.0,    "MOS"),  # v2: floor 2.5 → 3.0; target 3.0 → 3.5
    ("dnsmos_ovrl",           3.0,     2.7,    "MOS"),
    ("aecmos_echo",           4.3,     4.0,    "MOS"),  # v2: floor 3.5 → 4.0; target 4.0 → 4.3
    ("aecmos_dt",             3.5,     3.0,    "MOS"),
]


def _parse_optional_float(s: str) -> float | None:
    """Parse a CSV cell that may be empty or NaN."""
    if s is None or s == "" or s.lower() == "nan":
        return None
    try:
        v = float(s)
    except ValueError:
        return None
    if math.isnan(v):
        return None
    return v


def grade_row(row: dict[str, str]) -> tuple[list[str], list[str]]:
    """Grade one CSV row. Returns (floor_misses, soft_target_misses) as
    lists of human-readable strings. Missing metrics (NaN / unset) count
    as neither pass nor fail — they're informational gaps."""
    fails: list[str] = []
    warns: list[str] = []
    for col, tgt, floor, unit in BAR:
        v = _parse_optional_float(row.get(col, ""))
        if v is None:
            warns.append(f"{col:22s} = N/A (column missing or NaN)")
            continue
        if v < floor:
            fails.append(f"{col:22s} = {v:6.2f} {unit:3s}  <  floor {floor:4.1f}")
        elif v < tgt:
            warns.append(f"{col:22s} = {v:6.2f} {unit:3s}  <  target {tgt:4.1f} "
                         f"(floor {floor:4.1f} met)")
    return fails, warns


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in-csv", required=True, type=Path,
                    help="output of score_mos.py (must have dnsmos_* / aecmos_* columns)")
    args = ap.parse_args()

    if not args.in_csv.exists():
        print(f"--in-csv does not exist: {args.in_csv}", file=sys.stderr)
        return 1

    any_fail = False
    any_warn = False
    n_rows = 0
    with open(args.in_csv, newline="") as fin:
        reader = csv.DictReader(fin)
        if reader.fieldnames is None:
            print("--in-csv has no header row", file=sys.stderr)
            return 1
        print(f"Phase-1 acceptance bar (ADR-0012) against {args.in_csv}")
        print(f"{'=' * 72}")
        for row in reader:
            n_rows += 1
            cid = row.get("condition_id", "<unknown>")
            fails, warns = grade_row(row)
            if fails:
                any_fail = True
                print(f"\n[FAIL] condition '{cid}' missed {len(fails)} floor(s):")
                for line in fails:
                    print(f"  {line}")
                if warns:
                    print(f"       also missed {len(warns)} soft target(s):")
                    for line in warns:
                        print(f"  {line}")
            elif warns:
                any_warn = True
                print(f"\n[WARN] condition '{cid}' met all floors, missed {len(warns)} target(s):")
                for line in warns:
                    print(f"  {line}")
            else:
                print(f"\n[PASS] condition '{cid}' met all targets")
    print()
    print(f"Summary: {n_rows} condition(s) graded.")
    if any_fail:
        print("Overall: BLOCK — Phase 1 not ready for lab.")
        return 1
    if any_warn:
        print("Overall: WARN — all floors met; soft targets pending.")
        return 2
    print("Overall: GREEN — Phase 1 acceptance bar met across all conditions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
