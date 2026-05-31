#!/usr/bin/env python3
"""Sweep NS blend configurations against the ADR-0012 acceptance gate.

Two modes:

  Single-fixture (legacy): one (mic, ref) pair, prints a per-config comparison
  table + per-metric winner ranking.

  Corpus mode (--manifest): iterates the 30-clip AEC-Challenge subset for each
  config, aggregates per-(config, scenario, metric) percentiles, prints a
  per-scenario per-config table.

Sister of reference/score_mos.py: where score_mos consumes ecnr_eval's
per-condition CSV, this script consumes either a single (mic, ref) pair OR
a manifest of clips, and varies the chain config across runs.

Usage (single-fixture):
  python3 reference/sweep_ns_blend.py \\
      --mic reference/mixed_sound.wav \\
      --ref reference/reference_sound_to_be_eliminated.wav \\
      --bench ./build/ecnr_bench \\
      --dnsmos-model models/dnsmos_p835.onnx \\
      --aecmos-model models/aecmos.onnx \\
      --out-csv /tmp/sweep_results.csv \\
      [--out-wavs /tmp/sweep]

Usage (corpus mode):
  python3 reference/sweep_ns_blend.py \\
      --manifest datasets/aec_challenge/MANIFEST.tsv \\
      --datasets-root datasets/aec_challenge \\
      --bench ./build/ecnr_bench \\
      --dnsmos-model models/dnsmos_p835.onnx \\
      --aecmos-model models/aecmos.onnx \\
      --out-dir /tmp/sweep_corpus

Output:
  Stdout: ANSI-coloured tables + ADR-0012 floor verdicts.
  Single-fixture: --out-csv (one row per swept config).
  Corpus mode: <out-dir>/per_clip.csv (config × clip rows) + summary.csv
  (config × scenario × metric percentiles).
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import math
import subprocess
import sys
from pathlib import Path

try:
    import numpy as np  # type: ignore
    import onnxruntime as ort  # type: ignore
except ImportError as e:
    print(f"requires numpy + onnxruntime: {e}", file=sys.stderr)
    sys.exit(2)


# NS-blend sweep — no-mitigation baseline, Step A (uniform blend),
# Step B (VAD-gated blend across the full ladder), NS-off ceiling.
NS_CONFIGS = [
    ("rnnoise_default", []),
    ("step_a_blend_15", ["--ns-dry-blend", "0.15"]),
    ("step_a_blend_25", ["--ns-dry-blend", "0.25"]),
    ("step_b_vad_0p30", ["--ns-vad-blend", "0.0,0.30"]),
    ("step_b_vad_0p50", ["--ns-vad-blend", "0.0,0.50"]),
    ("step_b_vad_0p70", ["--ns-vad-blend", "0.0,0.70"]),
    ("step_b_vad_0p85", ["--ns-vad-blend", "0.0,0.85"]),
    ("step_b_vad_1p00", ["--ns-vad-blend", "0.0,1.00"]),
    ("ns_off_ceiling",  ["--ns-dry-blend", "1.0"]),
]

# AGC max_gain_db sweep — AGC off baseline + AGC on at five gain caps.
# Default WebRTC value is 50; we span 20-80 to find the operating point
# that maximises dnsmos_sig without over-brightening background noise.
AGC_CONFIGS = [
    ("agc_off",             []),
    ("agc_on_max_20",       ["--agc", "--agc-max-gain-db", "20"]),
    ("agc_on_max_30",       ["--agc", "--agc-max-gain-db", "30"]),
    ("agc_on_max_40",       ["--agc", "--agc-max-gain-db", "40"]),
    ("agc_on_max_50_def",   ["--agc"]),  # WebRTC default
    ("agc_on_max_60",       ["--agc", "--agc-max-gain-db", "60"]),
    ("agc_on_max_80",       ["--agc", "--agc-max-gain-db", "80"]),
]

CONFIG_SETS = {"ns": NS_CONFIGS, "agc": AGC_CONFIGS}
DEFAULT_CONFIGS = NS_CONFIGS  # back-compat alias for the original sweep behaviour

# Scenario → AECMOS talk_type + applicable AECMOS columns.
# Same shape as run_aec_challenge.py SCENARIO_MAP.
SCENARIO_MAP = {
    "doubletalk":         {"talk_type": "dt",
                           "aecmos_cols": ("aecmos_echo", "aecmos_other", "aecmos_dt")},
    "nearend_singletalk": {"talk_type": "st_ne",
                           "aecmos_cols": ("aecmos_other",)},
    "farend_singletalk":  {"talk_type": "st_fe",
                           "aecmos_cols": ("aecmos_echo",)},
}

# ADR-0012 §2 floors + soft targets.
FLOORS = {"dnsmos_sig": 3.0, "dnsmos_bak": 2.5, "dnsmos_ovrl": 2.7,
          "aecmos_echo": 3.5, "aecmos_dt": 3.0}
TARGETS = {"dnsmos_sig": 3.5, "dnsmos_bak": 3.0, "dnsmos_ovrl": 3.0,
           "aecmos_echo": 4.0, "aecmos_dt": 3.5}


def colour(v: float, floor: float, target: float) -> str:
    if v < floor:  return f"\033[31m{v:6.2f}\033[0m"
    if v < target: return f"\033[33m{v:6.2f}\033[0m"
    return f"\033[32m{v:6.2f}\033[0m"


def _load_score_mos():
    """Import reference/score_mos.py by path (avoids package install)."""
    spec = importlib.util.spec_from_file_location(
        "score_mos", Path(__file__).parent / "score_mos.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore
    return mod


def run_corpus_sweep(args, configs, scoremos, dn_sess, ae_sess) -> int:
    """Iterate {config × clip} across the AEC-Challenge manifest.
    Aggregates per-(config, scenario, metric); writes per_clip.csv + summary.csv."""
    if not args.manifest.exists():
        print(f"--manifest not found: {args.manifest}", file=sys.stderr); return 2
    with open(args.manifest) as f:
        clips = list(csv.DictReader(f, delimiter="\t"))
    args.out_dir.mkdir(parents=True, exist_ok=True)

    per_clip = []  # one row per (config, clip)
    for cfg_name, extra in configs:
        cfg_enh_dir = args.out_dir / cfg_name
        cfg_enh_dir.mkdir(exist_ok=True)
        print(f"\n=== {cfg_name} ===")
        for r in clips:
            scn = r["scenario"]
            mic_p = args.datasets_root / r["mic_filename"]
            ref_p = args.datasets_root / r["ref_filename"]
            enh_p = cfg_enh_dir / f"{r['clip_id']}_enh.wav"
            cmd = [str(args.bench), "--mic", str(mic_p), "--ref", str(ref_p),
                   "--out", str(enh_p), "--bypass-beamformer"]
            cmd.extend(extra)
            result = subprocess.run(cmd, capture_output=True, text=True)
            row = {"config": cfg_name, "clip_id": r["clip_id"], "scenario": scn,
                   "dnsmos_sig": math.nan, "dnsmos_bak": math.nan, "dnsmos_ovrl": math.nan,
                   "aecmos_echo": math.nan, "aecmos_other": math.nan, "aecmos_dt": math.nan,
                   "status": "ok"}
            if result.returncode != 0:
                row["status"] = "bench_failed"
                print(f"  {r['clip_id']:<10} {scn:<22} BENCH FAILED", file=sys.stderr)
                per_clip.append(row)
                continue
            try:
                enh, fs_e = scoremos._read_mono_float(enh_p)
                mic_sig, _ = scoremos._read_mono_float(mic_p)
                ref_sig, _ = scoremos._read_mono_float(ref_p)
                s, b, o = scoremos.score_dnsmos_p835(enh, fs_e, dn_sess)
                row["dnsmos_sig"], row["dnsmos_bak"], row["dnsmos_ovrl"] = s, b, o
                tt = SCENARIO_MAP[scn]["talk_type"]
                e, oth, dt = scoremos.score_aecmos(ref_sig, mic_sig, enh, fs_e, ae_sess,
                                                    talk_type=tt)
                cols = SCENARIO_MAP[scn]["aecmos_cols"]
                if "aecmos_echo"  in cols: row["aecmos_echo"]  = e
                if "aecmos_other" in cols: row["aecmos_other"] = oth
                if "aecmos_dt"    in cols: row["aecmos_dt"]    = dt
            except Exception as ex:
                row["status"] = "score_failed"
                print(f"  {r['clip_id']:<10} {scn:<22} SCORE FAILED: {ex}", file=sys.stderr)
            per_clip.append(row)
            if row["status"] == "ok":
                print(f"  {r['clip_id']:<10} {scn:<22} "
                      f"sig={row['dnsmos_sig']:.2f} bak={row['dnsmos_bak']:.2f} "
                      f"echo={row['aecmos_echo']:.2f} other={row['aecmos_other']:.2f}")

    # Aggregate per (config, scenario, metric).
    summary = []
    metrics = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
               "aecmos_echo", "aecmos_other", "aecmos_dt"]
    for cfg_name, _ in configs:
        for scn in sorted({c["scenario"] for c in per_clip}):
            rows_ = [c for c in per_clip
                     if c["config"] == cfg_name and c["scenario"] == scn
                     and c["status"] == "ok"]
            for m in metrics:
                vals = [c[m] for c in rows_
                        if isinstance(c[m], (int, float)) and not math.isnan(c[m])]
                if not vals: continue
                arr = np.array(vals)
                p10, p50, p90 = np.percentile(arr, [10, 50, 90])
                floor = FLOORS.get(m); target = TARGETS.get(m)
                summary.append({
                    "config": cfg_name, "scenario": scn, "metric": m,
                    "n_clips": len(vals), "p10": p10, "p50": p50, "p90": p90,
                    "n_below_floor":  sum(1 for v in vals if floor  is not None and v < floor),
                    "n_below_target": sum(1 for v in vals if target is not None and v < target),
                })

    # Write outputs.
    pc_cols = ["config", "clip_id", "scenario",
               "dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
               "aecmos_echo", "aecmos_other", "aecmos_dt", "status"]
    with open(args.out_dir / "per_clip.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=pc_cols, extrasaction="ignore")
        w.writeheader()
        for r in per_clip:
            w.writerow({k: (f"{r[k]:.3f}" if isinstance(r.get(k), float) and not math.isnan(r[k])
                            else ("" if isinstance(r.get(k), float) and math.isnan(r[k])
                                  else r.get(k, "")))
                        for k in pc_cols})
    sum_cols = ["config", "scenario", "metric", "n_clips",
                "p10", "p50", "p90", "n_below_floor", "n_below_target"]
    with open(args.out_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sum_cols)
        w.writeheader()
        for s in summary:
            w.writerow({k: (f"{s[k]:.3f}" if isinstance(s[k], float) else s[k])
                        for k in sum_cols})

    # Stdout per-scenario per-config table: shows p50 of each metric per config.
    for scn in sorted({s["scenario"] for s in summary}):
        print(f"\n=== {scn} (p50 per config) ===")
        applicable = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl"]
        scn_cols = SCENARIO_MAP[scn]["aecmos_cols"]
        if "aecmos_echo"  in scn_cols: applicable.append("aecmos_echo")
        if "aecmos_other" in scn_cols: applicable.append("aecmos_other")
        if "aecmos_dt"    in scn_cols: applicable.append("aecmos_dt")
        hdr = "  config".ljust(22) + "  ".join(m.rjust(13) for m in applicable) + "  verdict"
        print(hdr); print("-" * len(hdr))
        for cfg_name, _ in configs:
            cells = []
            fails = []
            for m in applicable:
                rec = next((s for s in summary if s["config"] == cfg_name
                            and s["scenario"] == scn and s["metric"] == m), None)
                if rec is None:
                    cells.append("       n/a"); continue
                cells.append(colour(rec["p50"], FLOORS.get(m, math.inf),
                                                  TARGETS.get(m, math.inf)).rjust(13))
                if m in FLOORS and rec["p50"] < FLOORS[m]:
                    fails.append(m)
            v = "PASS" if not fails else "BLOCK: " + ",".join(fails)
            print(f"  {cfg_name:<20}  " + "  ".join(cells) + f"  {v}")

    print(f"\nWrote {args.out_dir}/per_clip.csv, summary.csv")

    # Exit 0 if some config clears every floor on every applicable-metric × scenario.
    any_pass = False
    for cfg_name, _ in configs:
        if all((s["p50"] >= FLOORS[s["metric"]])
                for s in summary
                if s["config"] == cfg_name and s["metric"] in FLOORS):
            any_pass = True; break
    return 0 if any_pass else 1


def run_single_fixture(args, configs, scoremos, dn_sess, ae_sess) -> int:
    """Original single-pair sweep — preserved for backward-compat."""
    mic, fs_m = scoremos._read_mono_float(args.mic)
    ref, fs_r = scoremos._read_mono_float(args.ref)
    if fs_m != fs_r:
        print(f"sample rate mismatch: mic={fs_m} ref={fs_r}", file=sys.stderr); return 1

    args.out_wavs.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, extra in configs:
        out_wav = args.out_wavs / f"{name}.wav"
        cmd = [str(args.bench),
               "--mic", str(args.mic), "--ref", str(args.ref),
               "--out", str(out_wav)]
        if args.bypass_beamformer:
            cmd.append("--bypass-beamformer")
        cmd.extend(extra)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  {name:<22}  bench failed: {result.stderr}", file=sys.stderr)
            continue
        print(f"  {name:<22}  {result.stdout.strip().splitlines()[-1]}")
        enh, fs_e = scoremos._read_mono_float(out_wav)
        dn_sig, dn_bak, dn_ovr = scoremos.score_dnsmos_p835(enh, fs_e, dn_sess)
        ae_echo, ae_other, ae_dt = scoremos.score_aecmos_dt(ref, mic, enh, fs_e, ae_sess)
        rows.append({"name": name, "dnsmos_sig": dn_sig, "dnsmos_bak": dn_bak,
                     "dnsmos_ovrl": dn_ovr, "aecmos_echo": ae_echo,
                     "aecmos_other": ae_other, "aecmos_dt": ae_dt})

    # Comparison table
    print()
    hdr = ("config", "dnsmos_sig", "dnsmos_bak", "dnsmos_ovr", "aecmos_echo",
           "aecmos_other", "aecmos_dt", "floor verdict")
    print(f"{hdr[0]:<22}  {hdr[1]:>10}  {hdr[2]:>10}  {hdr[3]:>10}  "
          f"{hdr[4]:>11}  {hdr[5]:>12}  {hdr[6]:>9}  {hdr[7]:<14}")
    print("-" * 122)
    for r in rows:
        fails = [k + "↓" for k in FLOORS if r[k] < FLOORS[k]]
        verdict = "PASS" if not fails else "FAIL: " + ",".join(fails)
        print(f"  {r['name']:<20}  {colour(r['dnsmos_sig'],   FLOORS['dnsmos_sig'],   TARGETS['dnsmos_sig']):>16}  "
              f"{colour(r['dnsmos_bak'],   FLOORS['dnsmos_bak'],   TARGETS['dnsmos_bak']):>16}  "
              f"{colour(r['dnsmos_ovrl'],  FLOORS['dnsmos_ovrl'],  TARGETS['dnsmos_ovrl']):>16}  "
              f"{colour(r['aecmos_echo'],  FLOORS['aecmos_echo'],  TARGETS['aecmos_echo']):>17}  "
              f"{r['aecmos_other']:>12.2f}  "
              f"{colour(r['aecmos_dt'],    FLOORS['aecmos_dt'],    TARGETS['aecmos_dt']):>15}  "
              f"{verdict}")

    # Per-metric winner
    print("\nPer-metric winner:")
    for k in ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl", "aecmos_echo", "aecmos_dt"]:
        best = max(rows, key=lambda r: r[k])
        marker = " ✓ floor" if best[k] >= FLOORS[k] else " ✗ floor"
        print(f"  {k:14s}: {best[k]:.2f}  ({best['name']}){marker}")

    if args.out_csv:
        cols = ["name", "dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                "aecmos_echo", "aecmos_other", "aecmos_dt"]
        with open(args.out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            for r in rows:
                w.writerow({c: f"{r[c]:.3f}" if isinstance(r[c], float) else r[c]
                            for c in cols})
        print(f"\nWrote {args.out_csv}")

    # Exit 1 if NO config clears all floors — useful for CI / scripted gates.
    any_pass = any(all(r[k] >= FLOORS[k] for k in FLOORS) for r in rows)
    return 0 if any_pass else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    # Single-fixture mode args:
    ap.add_argument("--mic", type=Path, help="(single-fixture) mic capture WAV")
    ap.add_argument("--ref", type=Path, help="(single-fixture) far-end reference WAV")
    ap.add_argument("--out-wavs", default=Path("/tmp/sweep"), type=Path,
                    help="(single-fixture) per-config chain output WAVs dir")
    ap.add_argument("--out-csv", default=None, type=Path,
                    help="(single-fixture) optional machine-readable CSV")
    # Corpus mode args:
    ap.add_argument("--manifest", type=Path,
                    help="(corpus mode) AEC-Challenge MANIFEST.tsv path")
    ap.add_argument("--datasets-root", type=Path,
                    default=Path("datasets/aec_challenge"),
                    help="(corpus mode) directory holding the cached corpus WAVs")
    ap.add_argument("--out-dir", default=None, type=Path,
                    help="(corpus mode) directory for per_clip.csv + summary.csv")
    # Shared args:
    ap.add_argument("--bench", default=Path("./build/ecnr_bench"), type=Path)
    ap.add_argument("--dnsmos-model", required=True, type=Path)
    ap.add_argument("--aecmos-model", required=True, type=Path)
    ap.add_argument("--bypass-beamformer", action="store_true", default=True,
                    help="pass --bypass-beamformer to ecnr_bench (mono input)")
    ap.add_argument("--config-set", choices=list(CONFIG_SETS), default="ns",
                    help="which sweep config list to run (default: ns)")
    args = ap.parse_args()
    configs = CONFIG_SETS[args.config_set]

    # Mode detection — manifest takes precedence if both provided.
    corpus_mode = args.manifest is not None
    if not corpus_mode and (args.mic is None or args.ref is None):
        ap.error("must provide either --manifest OR both --mic and --ref")
    if corpus_mode and args.out_dir is None:
        args.out_dir = Path("/tmp/sweep_corpus")

    if not args.bench.exists():
        print(f"--bench {args.bench} not found", file=sys.stderr); return 1

    scoremos = _load_score_mos()
    dn_sess = ort.InferenceSession(str(args.dnsmos_model),
                                    providers=["CPUExecutionProvider"])
    ae_sess = ort.InferenceSession(str(args.aecmos_model),
                                    providers=["CPUExecutionProvider"])

    return run_corpus_sweep(args, configs, scoremos, dn_sess, ae_sess) if corpus_mode \
        else run_single_fixture(args, configs, scoremos, dn_sess, ae_sess)


if __name__ == "__main__":
    raise SystemExit(main())
