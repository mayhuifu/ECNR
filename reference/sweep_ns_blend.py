#!/usr/bin/env python3
"""Sweep NS blend configurations against the ADR-0012 acceptance gate.

For a given (mic, ref) pair, runs ecnr_bench across a battery of NS-blend
configurations, scores each output with DNSMOS P.835 + AECMOS, and prints a
per-config table + per-metric winner ranking + ADR-0012 floor verdict.

Sister of reference/score_mos.py: where score_mos consumes ecnr_eval's
per-condition CSV, this script consumes a single (mic, ref) pair and varies
the chain config across runs — the natural shape for tuning a single knob
against the gate.

Usage:
  python3 reference/sweep_ns_blend.py \\
      --mic reference/mixed_sound.wav \\
      --ref reference/reference_sound_to_be_eliminated.wav \\
      --bench ./build/ecnr_bench \\
      --dnsmos-model models/dnsmos_p835.onnx \\
      --aecmos-model models/aecmos.onnx \\
      --out-csv /tmp/sweep_results.csv \\
      [--out-wavs /tmp/sweep]

Output:
  Stdout: an ANSI-coloured comparison table + per-metric winner list +
  ADR-0012 floor verdict per config.

  --out-csv: machine-readable CSV with one row per swept config + the six
  ADR-0012 metric columns.
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import subprocess
import sys
from pathlib import Path

try:
    import onnxruntime as ort  # type: ignore
except ImportError:
    print("requires onnxruntime: pip install onnxruntime", file=sys.stderr)
    sys.exit(2)


# Default sweep — covers no-mitigation baseline, Step A (uniform blend),
# Step B (VAD-gated blend across the full ladder), and NS-off ceiling.
DEFAULT_CONFIGS = [
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

# ADR-0012 §2 floors + soft targets.
FLOORS = {"dnsmos_sig": 3.0, "dnsmos_bak": 2.5, "dnsmos_ovrl": 2.7,
          "aecmos_echo": 3.5, "aecmos_dt": 3.0}
TARGETS = {"dnsmos_sig": 3.5, "dnsmos_bak": 3.0, "dnsmos_ovrl": 3.0,
           "aecmos_echo": 4.0, "aecmos_dt": 3.5}


def colour(v: float, floor: float, target: float) -> str:
    if v < floor:  return f"\033[31m{v:6.2f}\033[0m"
    if v < target: return f"\033[33m{v:6.2f}\033[0m"
    return f"\033[32m{v:6.2f}\033[0m"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mic",   required=True, type=Path)
    ap.add_argument("--ref",   required=True, type=Path)
    ap.add_argument("--bench", default=Path("./build/ecnr_bench"), type=Path)
    ap.add_argument("--dnsmos-model", required=True, type=Path)
    ap.add_argument("--aecmos-model", required=True, type=Path)
    ap.add_argument("--out-wavs", default=Path("/tmp/sweep"), type=Path,
                    help="dir for per-config chain output WAVs")
    ap.add_argument("--out-csv", default=None, type=Path,
                    help="optional machine-readable CSV of results")
    ap.add_argument("--bypass-beamformer", action="store_true", default=True,
                    help="pass --bypass-beamformer to ecnr_bench (mono input)")
    args = ap.parse_args()

    if not args.bench.exists():
        print(f"--bench {args.bench} not found", file=sys.stderr); return 1
    args.out_wavs.mkdir(parents=True, exist_ok=True)

    # Import score_mos by file path so this script doesn't require a package install.
    spec = importlib.util.spec_from_file_location(
        "score_mos", Path(__file__).parent / "score_mos.py")
    score_mos = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(score_mos)  # type: ignore

    # Load shared inputs once.
    mic, fs_m = score_mos._read_mono_float(args.mic)
    ref, fs_r = score_mos._read_mono_float(args.ref)
    if fs_m != fs_r:
        print(f"sample rate mismatch: mic={fs_m} ref={fs_r}", file=sys.stderr); return 1

    dn_sess = ort.InferenceSession(str(args.dnsmos_model),
                                    providers=["CPUExecutionProvider"])
    ae_sess = ort.InferenceSession(str(args.aecmos_model),
                                    providers=["CPUExecutionProvider"])

    rows = []
    for name, extra in DEFAULT_CONFIGS:
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
        enh, fs_e = score_mos._read_mono_float(out_wav)
        dn_sig, dn_bak, dn_ovr = score_mos.score_dnsmos_p835(enh, fs_e, dn_sess)
        ae_echo, ae_other, ae_dt = score_mos.score_aecmos_dt(ref, mic, enh, fs_e, ae_sess)
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


if __name__ == "__main__":
    raise SystemExit(main())
