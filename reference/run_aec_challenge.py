#!/usr/bin/env python3
"""Run the ECNR chain against the AEC-Challenge 30-clip subset; produce
a perceptual-quality report.

Per-clip: ecnr_bench → AECMOS (talk_type per scenario) + DNSMOS P.835.
Aggregates per-scenario p10/p50/p90 + floor/target counts against the
ADR-0012 acceptance bar.

Per design spec
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import math
import os
import shlex
import subprocess
import sys
from pathlib import Path

try:
    import numpy as np  # type: ignore
    import onnxruntime as ort  # type: ignore
except ImportError as e:
    print(f"requires numpy + onnxruntime: {e}", file=sys.stderr)
    sys.exit(2)

# ---- Scenario → AECMOS talk_type + meaningful columns ----------------
# `aecmos_cols`         — AECMOS columns to populate (non-NaN) for this scenario
# `enforced_metrics`    — per ADR-0012 §3.1 applicability matrix: which metrics
#                         contribute to floor enforcement / CORPUS VERDICT.
#                         ST_FE excludes dnsmos_sig/ovrl (no near-end content);
#                         ST_NE excludes aecmos_echo/dt (no far-end content).
SCENARIO_MAP = {
    "doubletalk": {
        "talk_type":         "dt",
        "aecmos_cols":       ("aecmos_echo", "aecmos_other", "aecmos_dt"),
        "enforced_metrics":  ("dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                              "aecmos_echo", "aecmos_dt"),
    },
    "nearend_singletalk": {
        "talk_type":         "st_ne",
        "aecmos_cols":       ("aecmos_other",),
        "enforced_metrics":  ("dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl"),
    },
    "farend_singletalk": {
        "talk_type":         "st_fe",
        "aecmos_cols":       ("aecmos_echo",),
        # ST_FE excludes dnsmos_sig / dnsmos_ovrl per ADR-0012 §3.1 — those
        # clips have no intended near-end speech, so SIG/OVRL measure
        # residual-leakage perception (informational) rather than chain damage.
        "enforced_metrics":  ("dnsmos_bak", "aecmos_echo"),
    },
}

# ADR-0012 §2.1 v2 (2026-05-31) — measured-baseline-informed floors + targets.
# Authoritative source: docs/adr/0012-phase-1-acceptance-bar.md §2.1.
# Mirror any change to reference/check_acceptance_bar.py.
FLOORS  = {"dnsmos_sig": 3.0, "dnsmos_bak": 3.0, "dnsmos_ovrl": 2.7,
           "aecmos_echo": 4.0, "aecmos_dt": 3.0}
TARGETS = {"dnsmos_sig": 3.3, "dnsmos_bak": 3.5, "dnsmos_ovrl": 3.0,
           "aecmos_echo": 4.3, "aecmos_dt": 3.5}

PER_CLIP_COLS = ["clip_id", "scenario",
                 "dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                 "aecmos_echo", "aecmos_other", "aecmos_dt",
                 "erle_reported_db", "cpu_ms_per_frame", "rtf", "status"]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_score_mos():
    """Import reference/score_mos.py by path (avoids requiring a package install)."""
    spec = importlib.util.spec_from_file_location(
        "score_mos", Path(__file__).parent / "score_mos.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_manifest(path: Path) -> list[dict]:
    if not path.exists():
        print(f"MANIFEST.tsv not found at {path} — run fetch_aec_challenge.py first",
              file=sys.stderr)
        sys.exit(2)
    with open(path) as f:
        return list(csv.DictReader(f, delimiter="\t"))


def verify_cache(rows: list[dict], root: Path) -> None:
    """Exit 2 with a useful message if any clip is missing or SHA-mismatched.
    Layout is flat (no scenario subdirs) — see spec Addendum A1."""
    missing = []
    for r in rows:
        for which in ("mic", "ref"):
            fn = r[f"{which}_filename"]
            dest = root / fn
            if not dest.exists() or sha256_of(dest) != r[f"sha256_{which}"]:
                missing.append(f"  {fn}")
    if missing:
        print(f"Cache miss / SHA mismatch on {len(missing)} files. Run:\n"
              f"  python3 reference/fetch_aec_challenge.py\n\n"
              f"Missing:\n" + "\n".join(missing[:10]) +
              (f"\n  ... {len(missing) - 10} more" if len(missing) > 10 else ""),
              file=sys.stderr)
        sys.exit(2)


def run_bench(bench: Path, mic: Path, ref: Path, out_wav: Path,
              agc: bool, extra_flags: str) -> dict:
    """Invoke ecnr_bench. Returns {erle_reported_db, cpu_ms_per_frame, rtf, status,
       stderr (on fail)}. Parses bench's last-line summary; tolerates extra columns."""
    cmd = [str(bench), "--mic", str(mic), "--ref", str(ref), "--out", str(out_wav),
           "--bypass-beamformer"]
    if agc:
        cmd.append("--agc")
    if extra_flags:
        cmd.extend(shlex.split(extra_flags))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return {"status": "bench_failed", "stderr": result.stderr,
                "erle_reported_db": math.nan,
                "cpu_ms_per_frame": math.nan, "rtf": math.nan}
    # Last non-empty stdout line is the summary, e.g.:
    # "frames=1226  audio=12.260s  cpu=0.251s  rtf=0.0204  ...  erle_db=7.09  dropped=0"
    lines = [L for L in result.stdout.splitlines() if L.strip()]
    if not lines:
        print(f"  bench stdout empty for {mic.name}", file=sys.stderr)
        return {"status": "bench_failed",
                "stderr": "empty stdout",
                "erle_reported_db": math.nan,
                "cpu_ms_per_frame": math.nan, "rtf": math.nan}
    line = lines[-1]
    parts = dict(p.split("=", 1) for p in line.split() if "=" in p)

    def _f(k, default=math.nan):
        try:
            return float(parts.get(k, default))
        except (ValueError, TypeError):
            return default

    # Bench prints "audio=12.260s" and "cpu=0.249s" — strip trailing 's'.
    def _f_s(k, default=math.nan):
        try:
            return float(parts.get(k, str(default)).rstrip("s"))
        except (ValueError, TypeError):
            return default
    audio_s = _f_s("audio")
    cpu_s = _f_s("cpu")
    # cpu_ms_per_frame = (cpu_s * 1000) / frames; bench's "frames" field
    frames = _f("frames")
    cpu_ms_per_frame = (cpu_s * 1000.0 / frames) if (not math.isnan(cpu_s) and frames > 0) else math.nan
    return {"status": "ok",
            "erle_reported_db": _f("erle_db"),
            "cpu_ms_per_frame": cpu_ms_per_frame,
            "rtf": _f("rtf")}


def score_clip(clip_row: dict, enh_wav: Path,
               scoremos, dn_sess, ae_sess,
               datasets_root: Path) -> dict:
    """Score one clip; returns dict with the six MOS columns.
    NaN for irrelevant AECMOS columns per SCENARIO_MAP."""
    out = {"dnsmos_sig": math.nan, "dnsmos_bak": math.nan, "dnsmos_ovrl": math.nan,
           "aecmos_echo": math.nan, "aecmos_other": math.nan, "aecmos_dt": math.nan,
           "status": clip_row.get("status", "ok")}
    if out["status"] != "ok" or not enh_wav.exists():
        return out
    scn = clip_row["scenario"]
    mic = datasets_root / clip_row["mic_filename"]   # flat layout, spec Addendum A1
    ref = datasets_root / clip_row["ref_filename"]
    try:
        enh, fs_e = scoremos._read_mono_float(enh_wav)
        mic_sig, _ = scoremos._read_mono_float(mic)
        ref_sig, _ = scoremos._read_mono_float(ref)
        # DNSMOS applies regardless of scenario.
        s, b, o = scoremos.score_dnsmos_p835(enh, fs_e, dn_sess)
        out["dnsmos_sig"], out["dnsmos_bak"], out["dnsmos_ovrl"] = s, b, o
        # AECMOS with scenario-specific talk_type.
        tt = SCENARIO_MAP[scn]["talk_type"]
        e, oth, dt = scoremos.score_aecmos(ref_sig, mic_sig, enh, fs_e, ae_sess,
                                           talk_type=tt)
        # NaN columns that don't apply to this scenario.
        cols = SCENARIO_MAP[scn]["aecmos_cols"]
        if "aecmos_echo"  in cols: out["aecmos_echo"]  = e
        if "aecmos_other" in cols: out["aecmos_other"] = oth
        if "aecmos_dt"    in cols: out["aecmos_dt"]    = dt
    except Exception as ex:
        print(f"  score_failed: {clip_row['clip_id']}: {ex}", file=sys.stderr)
        out["status"] = "score_failed"
    return out


def aggregate(per_clip: list[dict]) -> list[dict]:
    """Per-(scenario, metric) summary: n, p10/p50/p90, floor/target counts.
    Skips NaN values (irrelevant columns per scenario)."""
    metrics = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
               "aecmos_echo", "aecmos_other", "aecmos_dt"]
    out = []
    for scn in sorted({c["scenario"] for c in per_clip}):
        rows = [c for c in per_clip if c["scenario"] == scn]
        n_ok = sum(1 for c in rows if c["status"] == "ok")
        for m in metrics:
            vals = [float(c[m]) for c in rows
                    if c["status"] == "ok"
                    and isinstance(c.get(m), (int, float))
                    and not math.isnan(float(c[m]))]
            if not vals:
                continue
            arr = np.array(vals)
            p10, p50, p90 = np.percentile(arr, [10, 50, 90])
            floor = FLOORS.get(m)
            target = TARGETS.get(m)
            n_below_floor  = sum(1 for v in vals if floor  is not None and v < floor)
            n_below_target = sum(1 for v in vals if target is not None and v < target)
            out.append({
                "scenario": scn, "metric": m, "n_clips": len(vals),
                "p10": p10, "p50": p50, "p90": p90,
                "n_below_floor": n_below_floor, "n_below_target": n_below_target,
            })
    return out


def write_outputs(per_clip: list[dict], summary: list[dict],
                  out_dir: Path, args: argparse.Namespace) -> None:
    # per_clip.csv
    with open(out_dir / "per_clip.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=PER_CLIP_COLS, extrasaction="ignore")
        w.writeheader()
        for r in per_clip:
            w.writerow({k: (f"{r[k]:.3f}" if isinstance(r.get(k), float)
                                              and not math.isnan(r[k])
                            else ("" if isinstance(r.get(k), float) and math.isnan(r[k])
                                  else r.get(k, "")))
                        for k in PER_CLIP_COLS})
    # summary.csv
    sum_cols = ["scenario", "metric", "n_clips",
                "p10", "p50", "p90", "n_below_floor", "n_below_target"]
    with open(out_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sum_cols)
        w.writeheader()
        for r in summary:
            w.writerow({k: (f"{r[k]:.3f}" if isinstance(r[k], float) else r[k])
                        for k in sum_cols})
    # README.txt — provenance
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip() or "unknown"
    with open(out_dir / "README.txt", "w") as f:
        f.write(f"Invocation: {' '.join(shlex.quote(a) for a in sys.argv)}\n")
        f.write(f"git rev:    {rev}\n")
        f.write(f"bench:      {args.bench}\n")
        f.write(f"agc:        {args.agc}\n")
        f.write(f"bench_flags: {args.bench_flags}\n")
        f.write(f"manifest:   {args.manifest}\n")
        f.write(f"n_clips:    {len(per_clip)}\n")


def _colour(v: float, floor: float | None, target: float | None) -> str:
    if floor is None or target is None or math.isnan(v):
        return f"{v:6.2f}"
    if v < floor:
        return f"\033[31m{v:6.2f}\033[0m"
    if v < target:
        return f"\033[33m{v:6.2f}\033[0m"
    return f"\033[32m{v:6.2f}\033[0m"


def print_summary(per_clip: list[dict], summary: list[dict]) -> bool:
    """Print per-scenario tables + CORPUS VERDICT. Returns True if PASS.

    Per ADR-0012 §3.1, the CORPUS VERDICT only enforces floors on metrics
    listed in SCENARIO_MAP[scn]['enforced_metrics']. Metrics that are
    populated but not enforced (e.g. ST_FE's dnsmos_sig — no near-end
    content) are labelled "(info, not enforced)" and printed with their
    floor/target inline for diagnostic visibility, but do not flip
    CORPUS VERDICT to BLOCK.
    """
    corpus_pass = True
    for scn in sorted({s["scenario"] for s in summary}):
        enforced = set(SCENARIO_MAP[scn]["enforced_metrics"])
        scn_rows = [r for r in per_clip if r["scenario"] == scn]
        n_total = len(scn_rows)
        n_ok = sum(1 for r in scn_rows if r["status"] == "ok")
        n_failed = n_total - n_ok
        print(f"\n=== {scn} ===   ({n_total} clips, {n_ok} ok, {n_failed} failed)")
        print(f"  {'metric':<14} {'p10':>6} {'p50':>6} {'p90':>6}  "
              f"{'floor':>6} {'target':>7}  verdict")
        for s in [s for s in summary if s["scenario"] == scn]:
            m = s["metric"]
            floor, target = FLOORS.get(m), TARGETS.get(m)
            is_enforced = (m in enforced)
            floor_s  = f"{floor:>6.2f}"  if floor  is not None else "    - "
            target_s = f"{target:>7.2f}" if target is not None else "    -  "
            if floor is None:
                verdict = "(informational)"
            elif not is_enforced:
                verdict = ("PASS (info)" if s["p50"] >= floor
                           else f"{s['n_below_floor']}/{s['n_clips']} below floor (info)")
            elif s["p50"] >= floor:
                verdict = "PASS"
            else:
                verdict = f"{s['n_below_floor']}/{s['n_clips']} below floor"
            # Suppress colour for non-enforced rows so the eye tracks the
            # actual gate-blocking failures rather than the structural ones.
            if is_enforced and floor is not None:
                colour_args = (floor, target)
            else:
                colour_args = (None, None)
            print(f"  {m:<14} {_colour(s['p10'], *colour_args)} "
                  f"{_colour(s['p50'], *colour_args)} "
                  f"{_colour(s['p90'], *colour_args)}  "
                  f"{floor_s} {target_s}  {verdict}")
            if is_enforced and floor is not None and s["p50"] < floor:
                corpus_pass = False
        if n_failed >= 3:
            print(f"  \033[31mWARNING: {n_failed} clips failed in {scn}\033[0m")
    print("\n" + ("=" * 50))
    verdict_str = "\033[32mPASS\033[0m" if corpus_pass else "\033[31mBLOCK\033[0m"
    print(f"CORPUS VERDICT: {verdict_str}")
    print("=" * 50)
    return corpus_pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", default=Path("./build/ecnr_bench"), type=Path)
    ap.add_argument("--manifest", default=Path("datasets/aec_challenge/MANIFEST.tsv"),
                    type=Path)
    ap.add_argument("--datasets-root", default=Path("datasets/aec_challenge"), type=Path)
    ap.add_argument("--dnsmos-model", required=True, type=Path)
    ap.add_argument("--aecmos-model", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--agc", action="store_true", help="pass --agc to ecnr_bench")
    ap.add_argument("--bench-flags", default="",
                    help="extra flags to pass through to ecnr_bench")
    ap.add_argument("--keep-enh-wavs", action="store_true", default=True)
    ap.add_argument("--no-keep-enh-wavs", dest="keep_enh_wavs", action="store_false")
    args = ap.parse_args()

    if not args.bench.exists():
        print(f"--bench not found: {args.bench}", file=sys.stderr); return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(args.manifest)
    verify_cache(rows, args.datasets_root)
    print(f"OK: {len(rows)} clips verified in cache")

    scoremos = load_score_mos()
    dn_sess = ort.InferenceSession(str(args.dnsmos_model),
                                    providers=["CPUExecutionProvider"])
    ae_sess = ort.InferenceSession(str(args.aecmos_model),
                                    providers=["CPUExecutionProvider"])

    per_clip = []
    for r in rows:
        scn = r["scenario"]
        mic = args.datasets_root / r["mic_filename"]   # flat layout, spec Addendum A1
        ref = args.datasets_root / r["ref_filename"]
        enh = args.out_dir / f"{r['clip_id']}_enh.wav"  # top-level per spec
        b = run_bench(args.bench, mic, ref, enh, args.agc, args.bench_flags)
        s = score_clip({**r, "status": b["status"]}, enh,
                        scoremos, dn_sess, ae_sess, args.datasets_root)
        row = {**r, **b, **s}
        per_clip.append(row)
        # Compact stdout line
        ok = row["status"] == "ok"
        marker = "+" if ok else "x"
        if ok:
            print(f"  {marker} {r['clip_id']:<14} {scn:<22}  "
                  f"sig={row['dnsmos_sig']:.2f}  bak={row['dnsmos_bak']:.2f}  "
                  f"echo={row['aecmos_echo']:.2f}  other={row['aecmos_other']:.2f}")
        else:
            print(f"  {marker} {r['clip_id']:<14} {scn:<22}  ({row['status']})",
                  file=sys.stderr)
    print(f"\nScored {sum(1 for c in per_clip if c['status']=='ok')}/{len(per_clip)} clips")

    summary = aggregate(per_clip)
    write_outputs(per_clip, summary, args.out_dir, args)
    print(f"\nWrote {args.out_dir}/per_clip.csv, summary.csv, README.txt")
    if not args.keep_enh_wavs:
        for r in per_clip:
            enh = args.out_dir / f"{r['clip_id']}_enh.wav"
            if enh.exists():
                enh.unlink()
    pass_ok = print_summary(per_clip, summary)
    # Per error-handling matrix: exit 1 if any scenario has all clips failed.
    failed_scenarios = []
    for scn in {r["scenario"] for r in per_clip}:
        scn_rows = [r for r in per_clip if r["scenario"] == scn]
        if scn_rows and all(r["status"] != "ok" for r in scn_rows):
            failed_scenarios.append(scn)
    if failed_scenarios:
        print(f"\nERROR: all clips failed in scenario(s): {failed_scenarios}",
              file=sys.stderr)
        return 1
    # Exit 0 on PASS, 1 on BLOCK, 2 on cache/fetch issues (handled earlier).
    return 0 if pass_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
