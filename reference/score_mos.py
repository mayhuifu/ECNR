#!/usr/bin/env python3
"""Post-process `ecnr_eval --run` output with AECMOS + DNSMOS columns.

ecnr_eval emits an ERLE-and-frame-accounting CSV per ADR-0011 §4. This
script augments that CSV with the perceptual MOS columns that the Phase-1
acceptance bar (ADR-0012) will be measured against:

  Column              Source                Range  What it measures
  ------------------  --------------------  -----  -----------------------------
  dnsmos_sig          DNSMOS P.835 SIG       1-5  speech quality alone (S-MOS)
  dnsmos_bak          DNSMOS P.835 BAK       1-5  background noise quality (N-MOS)
  dnsmos_ovrl         DNSMOS P.835 OVRL      1-5  overall quality (G-MOS)
  aecmos_echo         AECMOS echo            1-5  residual echo perception
  aecmos_other        AECMOS other           1-5  near-end voice degradation
  aecmos_dt           AECMOS doubletalk      1-5  doubletalk handling

Reuses the chain output WAVs that `ecnr_eval --run` already produced under
`<out_dir>/`. Per-row inputs come from the matching condition directory
(`mic.wav`, `ref.wav`, optional `near_end_clean.wav`).

Model files (download separately, see README):
  --dnsmos-model   path to Microsoft DNSMOS P.835 ONNX (sig_bak_ovr.onnx,
                   from github.com/microsoft/DNS-Challenge)
  --aecmos-model   path to Microsoft AECMOS ONNX (commit-specific,
                   from github.com/microsoft/AEC-Challenge)

When a model is not supplied, its columns are filled with NaN and a one-
line warning is printed. This lets the harness still run (e.g., for
schema-only smoke tests) without forcing every developer to download
hundreds of MB of model assets.

Usage:
  python3 reference/score_mos.py \\
    --in-csv     /tmp/eval/results.csv \\
    --out-csv    /tmp/eval/results_with_mos.csv \\
    --conditions conditions/synthetic \\
    --out-wavs   /tmp/eval \\
    [--dnsmos-model models/dnsmos_p835.onnx]

Dependencies: numpy, scipy (already project deps via reference/*.py),
onnxruntime (pip install onnxruntime).
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
import wave
from pathlib import Path

import numpy as np  # type: ignore
from scipy import signal  # type: ignore
from scipy.io import wavfile  # type: ignore

try:
    import onnxruntime as ort  # type: ignore
except ImportError:
    print("score_mos.py requires onnxruntime. Install via: pip install onnxruntime", file=sys.stderr)
    sys.exit(2)


# ---- DNSMOS P.835 ------------------------------------------------------------
# Reference: github.com/microsoft/DNS-Challenge/tree/master/DNSMOS
#
# Input: 16 kHz mono audio, ≥9 s. Shorter clips are tile-padded; longer are
# windowed at 9 s with 1-s hop and the mean of per-window predictions is
# reported. Feature: log-magnitude STFT with n_fft=320, hop=160 (10 ms hop
# at 16 kHz), magnitude only. Model output: 3 floats per inference —
# SIG, BAK, OVRL — already on the 1-5 MOS scale.

DNSMOS_FS = 16000
DNSMOS_FRAME = 320  # n_fft
DNSMOS_HOP = 160    # hop = 10 ms @ 16 kHz
DNSMOS_WINDOW_S = 9.0


def _resample_to_16k(x: np.ndarray, fs: int) -> np.ndarray:
    if fs == DNSMOS_FS:
        return x.astype(np.float32)
    # Polyphase resampler — quality good enough for MOS feature extraction;
    # the model wasn't trained on a specific resampler so we don't need
    # exact-fidelity downsampling.
    up = DNSMOS_FS
    down = fs
    g = math.gcd(up, down)
    up //= g
    down //= g
    return signal.resample_poly(x.astype(np.float32), up, down).astype(np.float32)


def _read_mono_float(path: Path) -> tuple[np.ndarray, int]:
    fs, raw = wavfile.read(str(path))
    if raw.ndim == 2:
        raw = raw.mean(axis=1)
    if raw.dtype == np.int16:
        x = raw.astype(np.float32) / 32768.0
    elif raw.dtype == np.int32:
        x = raw.astype(np.float32) / 2147483648.0
    elif raw.dtype == np.float32:
        x = raw
    else:
        x = raw.astype(np.float32)
    return x, fs


def _log_stft_magnitude(x: np.ndarray) -> np.ndarray:
    """Log-magnitude STFT compatible with the DNSMOS P.835 input pipeline."""
    f, t, Z = signal.stft(
        x,
        fs=DNSMOS_FS,
        nperseg=DNSMOS_FRAME,
        noverlap=DNSMOS_FRAME - DNSMOS_HOP,
        nfft=DNSMOS_FRAME,
        boundary=None,
        padded=False,
        return_onesided=True,
    )
    mag = np.abs(Z).astype(np.float32)
    # DNSMOS uses log(mag + tiny epsilon) to keep silent windows finite.
    return np.log(mag + 1e-7)


def score_dnsmos_p835(audio: np.ndarray, fs: int, session: ort.InferenceSession) -> tuple[float, float, float]:
    """Run DNSMOS P.835 on a single audio clip; return (SIG, BAK, OVRL)."""
    x = _resample_to_16k(audio, fs)
    n_min = int(DNSMOS_WINDOW_S * DNSMOS_FS)
    if x.size < n_min:
        # Tile-pad short clips to 9 s — the standard DNSMOS reference fallback.
        x = np.tile(x, int(np.ceil(n_min / x.size)))[:n_min]
    # Single-window scoring (use 9 s of the centre of the clip). Longer
    # clips are not multi-window-averaged here for simplicity; the central
    # 9 s typically dominates the MOS for a steady-state condition. If
    # multi-window averaging is needed later, hop by 1 s and mean.
    centre = max(0, (x.size - n_min) // 2)
    win = x[centre : centre + n_min]
    feat = _log_stft_magnitude(win)  # shape: (n_freq, n_time)
    # Match DNSMOS input shape: (1, n_freq, n_time, 1) — channel-last 4D.
    feat = feat[np.newaxis, :, :, np.newaxis].astype(np.float32)
    # Input name is model-specific; the published DNSMOS model uses "input_1".
    in_name = session.get_inputs()[0].name
    out = session.run(None, {in_name: feat})
    # Output order in the published sig_bak_ovr.onnx model: [SIG, BAK, OVRL].
    sig, bak, ovrl = (float(v) for v in np.array(out[0]).reshape(-1)[:3])
    return sig, bak, ovrl


# ---- AECMOS ------------------------------------------------------------------
# Reference: github.com/microsoft/AEC-Challenge/tree/main/AECMOS
#
# AECMOS takes 3 audio streams (lpb / mic / enhanced) and emits 3 MOS scores
# (echo / other / doubletalk). Its specific feature pipeline depends on the
# released model version and is more involved than DNSMOS. We scaffold here
# without implementing the inference: when --aecmos-model is supplied, the
# script emits placeholder zeros + a one-line warning explaining how to
# complete the integration.
#
# To wire it up: read AECMOS-Challenge's `AECMOS_local.py` for the exact
# feature pipeline of the specific ONNX checkpoint chosen, then implement
# in score_aecmos() below.

def score_aecmos_stub(*_args, **_kwargs) -> tuple[float, float, float]:
    """AECMOS placeholder. Returns NaN for echo/other/doubletalk."""
    nan = float("nan")
    return nan, nan, nan


# ---- Main --------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in-csv",     required=True, type=Path, help="ecnr_eval output CSV")
    ap.add_argument("--out-csv",    required=True, type=Path, help="augmented output CSV")
    ap.add_argument("--conditions", required=True, type=Path,
                    help="root dir matching ecnr_eval --conditions")
    ap.add_argument("--out-wavs",   required=True, type=Path,
                    help="dir holding chain-output WAVs (typically the same --out target as ecnr_eval); "
                         "expects per-row outputs named <condition_id>.wav (drop into your eval flow)")
    ap.add_argument("--dnsmos-model", type=Path, default=None,
                    help="DNSMOS P.835 ONNX path (sig_bak_ovr.onnx). "
                         "If unset, DNSMOS columns are NaN.")
    ap.add_argument("--aecmos-model", type=Path, default=None,
                    help="AECMOS ONNX path. If unset, AECMOS columns are NaN. "
                         "Stub-only today; see module docstring.")
    args = ap.parse_args()

    if not args.in_csv.exists():
        print(f"--in-csv does not exist: {args.in_csv}", file=sys.stderr)
        return 1

    dnsmos_sess = None
    if args.dnsmos_model is not None:
        if not args.dnsmos_model.exists():
            print(f"--dnsmos-model not found: {args.dnsmos_model}", file=sys.stderr)
            return 1
        dnsmos_sess = ort.InferenceSession(str(args.dnsmos_model),
                                            providers=["CPUExecutionProvider"])
    else:
        print("WARN: --dnsmos-model unset; dnsmos_* columns will be NaN.", file=sys.stderr)
    if args.aecmos_model is not None and not args.aecmos_model.exists():
        print(f"--aecmos-model not found: {args.aecmos_model}", file=sys.stderr)
        return 1
    if args.aecmos_model is None:
        print("WARN: --aecmos-model unset; aecmos_* columns will be NaN.", file=sys.stderr)
    else:
        # The script structurally accepts the model path for forward
        # compatibility, but the inference pipeline isn't implemented yet
        # (see score_aecmos_stub docstring). Warn loudly so the user knows
        # the column will still be NaN despite the model path being set.
        print("WARN: --aecmos-model received but AECMOS inference is not "
              "implemented yet — see score_mos.py module docstring. "
              "aecmos_* columns will be NaN.", file=sys.stderr)

    extra_cols = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                  "aecmos_echo", "aecmos_other", "aecmos_dt"]

    n_rows = 0
    n_scored_dnsmos = 0
    with open(args.in_csv, newline="") as fin, open(args.out_csv, "w", newline="") as fout:
        reader = csv.DictReader(fin)
        if reader.fieldnames is None:
            print("--in-csv has no header row", file=sys.stderr)
            return 1
        fieldnames = list(reader.fieldnames) + extra_cols
        writer = csv.DictWriter(fout, fieldnames=fieldnames)
        writer.writeheader()
        for row in reader:
            n_rows += 1
            cid = row.get("condition_id", "")
            out_wav = args.out_wavs / f"{cid}.wav"
            # DNSMOS scores the chain OUTPUT (the enhanced signal) — that's
            # the speech the receiver hears.
            dn_sig = dn_bak = dn_ovrl = float("nan")
            if dnsmos_sess is not None and out_wav.exists():
                try:
                    audio, fs = _read_mono_float(out_wav)
                    dn_sig, dn_bak, dn_ovrl = score_dnsmos_p835(audio, fs, dnsmos_sess)
                    n_scored_dnsmos += 1
                except Exception as e:  # noqa: BLE001
                    print(f"WARN: DNSMOS failed on {out_wav.name}: {e}", file=sys.stderr)
            elif dnsmos_sess is not None:
                print(f"WARN: missing chain output for condition {cid}: {out_wav}",
                      file=sys.stderr)
            # AECMOS stub — currently always NaN. See module docstring for
            # how to complete the integration once a specific model is pinned.
            ae_echo, ae_other, ae_dt = score_aecmos_stub()
            row["dnsmos_sig"]   = f"{dn_sig:.3f}"   if not math.isnan(dn_sig)   else ""
            row["dnsmos_bak"]   = f"{dn_bak:.3f}"   if not math.isnan(dn_bak)   else ""
            row["dnsmos_ovrl"]  = f"{dn_ovrl:.3f}"  if not math.isnan(dn_ovrl)  else ""
            row["aecmos_echo"]  = f"{ae_echo:.3f}"  if not math.isnan(ae_echo)  else ""
            row["aecmos_other"] = f"{ae_other:.3f}" if not math.isnan(ae_other) else ""
            row["aecmos_dt"]    = f"{ae_dt:.3f}"    if not math.isnan(ae_dt)    else ""
            writer.writerow(row)
    print(f"wrote {args.out_csv}: {n_rows} rows total, {n_scored_dnsmos} with DNSMOS scores")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
