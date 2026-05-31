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

# librosa is required ONLY for AECMOS (its mel-spectrogram pipeline matches
# the reference impl bit-for-bit). DNSMOS uses scipy-only STFT, so the
# import is deferred to the first AECMOS call to keep the DNSMOS-only path
# free of the librosa dependency.
_librosa = None
def _ensure_librosa():
    global _librosa
    if _librosa is None:
        try:
            import librosa  # type: ignore
            _librosa = librosa
        except ImportError:
            print("AECMOS scoring requires librosa. Install via: pip install librosa",
                  file=sys.stderr)
            sys.exit(2)
    return _librosa


# ---- DNSMOS P.835 ------------------------------------------------------------
# Reference: github.com/microsoft/DNS-Challenge/blob/master/DNSMOS/dnsmos_local.py
#
# Input: RAW 16 kHz mono audio of exactly INPUT_LENGTH seconds (9.01 s →
# 144160 samples). Model accepts shape (1, N) — a 2-D batch of raw audio.
# Output: 3 floats per inference (raw SIG, BAK, OVRL) which are then
# polynomial-calibrated to 1-5 MOS scale via get_polyfit_val below.
# (Reference confusingly calls the feature pipeline `audio_melspec` but
# that's only used by the secondary P.808 model, NOT sig_bak_ovr.onnx.)

DNSMOS_FS = 16000
DNSMOS_WINDOW_S = 9.01
DNSMOS_INPUT_LEN = int(DNSMOS_FS * DNSMOS_WINDOW_S)  # 144160 samples

# Polynomial calibration coefficients for the non-personalized P.835 head,
# lifted verbatim from get_polyfit_val() in dnsmos_local.py.
_DNSMOS_P_SIG = [-0.08397278,  1.22083953,  0.0052439]
_DNSMOS_P_BAK = [-0.13166888,  1.60915514, -0.39604546]
_DNSMOS_P_OVR = [-0.06766283,  1.11546468,  0.04602535]


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


def _dnsmos_polyfit(sig: float, bak: float, ovr: float) -> tuple[float, float, float]:
    """Apply the non-personalized P.835 polynomial calibration. Maps raw
    model outputs to the calibrated 1-5 MOS scale."""
    return (float(np.polyval(_DNSMOS_P_SIG, sig)),
            float(np.polyval(_DNSMOS_P_BAK, bak)),
            float(np.polyval(_DNSMOS_P_OVR, ovr)))


def score_dnsmos_p835(audio: np.ndarray, fs: int, session: ort.InferenceSession) -> tuple[float, float, float]:
    """Run DNSMOS P.835 on a single audio clip; return calibrated (SIG, BAK, OVRL)."""
    x = _resample_to_16k(audio, fs)
    # Pad / truncate to exactly INPUT_LENGTH = 9.01 s of 16 kHz audio.
    # Tile-pad short clips (DNSMOS reference fallback); centre-crop long ones.
    if x.size < DNSMOS_INPUT_LEN:
        x = np.tile(x, int(np.ceil(DNSMOS_INPUT_LEN / x.size)))[:DNSMOS_INPUT_LEN]
    else:
        centre = (x.size - DNSMOS_INPUT_LEN) // 2
        x = x[centre : centre + DNSMOS_INPUT_LEN]
    # Model input shape: (1, N) — batch dim + raw audio dim.
    feat = x[np.newaxis, :].astype(np.float32)
    in_name = session.get_inputs()[0].name
    out = session.run(None, {in_name: feat})
    # Raw model outputs: 3 floats in [SIG, BAK, OVR] order.
    raw_sig, raw_bak, raw_ovr = (float(v) for v in np.array(out[0]).reshape(-1)[:3])
    # Polynomial-calibrate to 1-5 MOS.
    return _dnsmos_polyfit(raw_sig, raw_bak, raw_ovr)


# ---- AECMOS ------------------------------------------------------------------
# Reference: github.com/microsoft/AEC-Challenge/tree/main/AECMOS/AECMOS_local
#
# AECMOS takes 3 audio streams (lpb / mic / enhanced) and emits 2 MOS scores
# under a "talk_type" scenario marker (st / nst / dt). For our chain output
# we always score under talk_type='dt' (doubletalk) which is the worst case
# the chain has to handle. Outputs:
#
#   echo_mos  → ADR-0012 aecmos_echo column (residual echo perception)
#   deg_mos   → ADR-0012 aecmos_other column (near-end damage)
#   (avg)     → ADR-0012 aecmos_dt column (combined doubletalk handling;
#               mean of echo_mos and deg_mos. The AECMOS model itself
#               doesn't emit a separate "doubletalk overall" score, so
#               we synthesize one via mean; documented in the README.)
#
# Implementation ported from
# https://github.com/microsoft/AEC-Challenge/blob/main/AECMOS/AECMOS_local/aecmos.py
# Model config matches "Run_1663915512_Stage_0" specifically:
#   - 16 kHz, dft_size=512, hop=256, n_mels=160, log-power mel-spectrogram
#   - GRU hidden state h0 of shape (4, 1, 64)
#   - Needs scenario marker (st/nst/dt encoded as 2 binary tail planes)

AECMOS_SR = 16000
AECMOS_DFT = 512
AECMOS_HOP = 256
AECMOS_N_MELS = 160
AECMOS_MAX_LEN_S = 20  # clip duration cap (matches reference)
AECMOS_HIDDEN = (4, 1, 64)


def _aecmos_mel_transform(sample: np.ndarray) -> np.ndarray:
    """Mel-spectrogram pipeline matching the AECMOS reference impl.

    Returns shape (n_time, n_mels). Equivalent to:
        mel = librosa.feature.melspectrogram(y=sample, sr=16000,
              n_fft=513, hop_length=256, n_mels=160)
        mel = (librosa.power_to_db(mel, ref=np.max) + 40) / 40
        return mel.T
    """
    librosa = _ensure_librosa()
    mel = librosa.feature.melspectrogram(
        y=sample, sr=AECMOS_SR,
        n_fft=AECMOS_DFT + 1,           # 513 per reference (off-by-one intentional)
        hop_length=AECMOS_HOP,
        n_mels=AECMOS_N_MELS,
    )
    mel_db = (librosa.power_to_db(mel, ref=np.max) + 40) / 40
    return mel_db.T.astype(np.float32)


# Public talk_type names → Microsoft AECMOS internal names. We keep our
# clearer mnemonics on the public surface (per_clip.csv etc.) and translate
# at this boundary. Source: AECMOS/AECMOS_local/aecmos.py asserts
# talk_type ∈ {'nst','st','dt'}.
_TALK_TYPE_TO_MS = {"st_ne": "nst", "st_fe": "st", "dt": "dt"}


def score_aecmos(lpb: np.ndarray, mic: np.ndarray, enh: np.ndarray,
                 fs: int, session: ort.InferenceSession,
                 talk_type: str = "dt") -> tuple[float, float, float]:
    """Run AECMOS under the given talk_type.

    talk_type ∈ {'dt', 'st_ne', 'st_fe'} per AEC-Challenge convention.
    Returns (echo_mos, other_mos, dt_combined). For single-talk scenarios
    only one of echo_mos / other_mos is meaningful; the caller is
    responsible for NaN-ing the irrelevant column.
    """
    if talk_type not in _TALK_TYPE_TO_MS:
        raise ValueError(f"talk_type must be one of "
                         f"{list(_TALK_TYPE_TO_MS)}, got {talk_type!r}")
    ms = _TALK_TYPE_TO_MS[talk_type]
    # Derive the (ne_st, fe_st) flags per upstream:
    #   nst → ne_st=1, fe_st=0
    #   st  → ne_st=0, fe_st=1
    #   dt  → ne_st=0, fe_st=0
    ne_st = 1 if ms == "nst" else 0
    fe_st = 1 if ms == "st"  else 0

    # Resample to 16 kHz if needed.
    if fs != AECMOS_SR:
        lpb = _resample_to_16k(lpb, fs)
        mic = _resample_to_16k(mic, fs)
        enh = _resample_to_16k(enh, fs)
    # Match lengths to the shortest signal.
    n = min(len(lpb), len(mic), len(enh))
    lpb, mic, enh = lpb[:n], mic[:n], enh[:n]
    # Clip to max-len if too long.
    n_cap = AECMOS_MAX_LEN_S * AECMOS_SR
    if n > n_cap:
        lpb, mic, enh = lpb[:n_cap], mic[:n_cap], enh[:n_cap]

    # Mel features.
    lpb_f = _aecmos_mel_transform(lpb)
    mic_f = _aecmos_mel_transform(mic)
    enh_f = _aecmos_mel_transform(enh)

    # Scenario marker: two 20-row tail planes per stream. Plane 2 always zeros.
    # Plane 1 differs per stream:
    #   mic:  (1 - fe_st)   → 1 for dt/st_ne, 0 for st_fe
    #   lpb:  (1 - ne_st)   → 1 for dt/st_fe, 0 for st_ne
    #   enh:  1.0           → always
    n_mels = mic_f.shape[1]
    zeros = np.zeros((20, n_mels), dtype=np.float32)
    def _append(x, plane1_value: float):
        plane1 = np.full((20, n_mels), plane1_value, dtype=np.float32)
        return np.concatenate((x, plane1, zeros), axis=0)
    mic_f = _append(mic_f, 1.0 - fe_st)
    lpb_f = _append(lpb_f, 1.0 - ne_st)
    enh_f = _append(enh_f, 1.0)

    # Stack channel-first: (1, 3, n_time, n_mels).
    feats = np.stack((lpb_f, mic_f, enh_f)).astype(np.float32)
    feats = np.expand_dims(feats, axis=0)
    h0 = np.zeros(AECMOS_HIDDEN, dtype=np.float32)

    in_name = session.get_inputs()[0].name
    result = session.run([], {in_name: feats, "h0": h0})
    echo_mos = float(result[0][0])
    deg_mos = float(result[0][1])
    dt_combined = 0.5 * (echo_mos + deg_mos)
    return echo_mos, deg_mos, dt_combined


def score_aecmos_dt(lpb: np.ndarray, mic: np.ndarray, enh: np.ndarray,
                     fs: int, session: ort.InferenceSession) -> tuple[float, float, float]:
    """Back-compat shim. Use score_aecmos(..., talk_type='dt') in new code."""
    return score_aecmos(lpb, mic, enh, fs, session, talk_type="dt")


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
    aecmos_sess = None
    if args.aecmos_model is not None:
        if not args.aecmos_model.exists():
            print(f"--aecmos-model not found: {args.aecmos_model}", file=sys.stderr)
            return 1
        aecmos_sess = ort.InferenceSession(str(args.aecmos_model),
                                            providers=["CPUExecutionProvider"])
    else:
        print("WARN: --aecmos-model unset; aecmos_* columns will be NaN.", file=sys.stderr)

    extra_cols = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                  "aecmos_echo", "aecmos_other", "aecmos_dt"]

    n_rows = 0
    n_scored_dnsmos = 0
    n_scored_aecmos = 0
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
            cond_dir = args.conditions / cid
            mic_wav = cond_dir / "mic.wav"
            ref_wav = cond_dir / "ref.wav"

            # DNSMOS scores the chain OUTPUT alone (perceptual quality of
            # what the receiver hears).
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

            # AECMOS scores the chain under talk_type='dt' (doubletalk).
            # Requires the matching mic + ref WAVs from the condition dir
            # plus the chain output. Maps to ADR-0012 columns:
            #   aecmos_echo  ← echo_mos
            #   aecmos_other ← deg_mos
            #   aecmos_dt    ← mean(echo_mos, deg_mos) — synthesized
            #                  combined doubletalk score; the AECMOS model
            #                  doesn't emit one natively.
            ae_echo = ae_other = ae_dt = float("nan")
            if aecmos_sess is not None and out_wav.exists() and mic_wav.exists() and ref_wav.exists():
                try:
                    mic_sig, fs_mic = _read_mono_float(mic_wav)
                    ref_sig, fs_ref = _read_mono_float(ref_wav)
                    enh_sig, fs_enh = _read_mono_float(out_wav)
                    if not (fs_mic == fs_ref == fs_enh):
                        print(f"WARN: AECMOS sample-rate mismatch on {cid} "
                              f"(mic={fs_mic} ref={fs_ref} enh={fs_enh})",
                              file=sys.stderr)
                    else:
                        ae_echo, ae_other, ae_dt = score_aecmos_dt(
                            ref_sig, mic_sig, enh_sig, fs_mic, aecmos_sess)
                        n_scored_aecmos += 1
                except Exception as e:  # noqa: BLE001
                    print(f"WARN: AECMOS failed on {cid}: {e}", file=sys.stderr)
            elif aecmos_sess is not None:
                missing = []
                if not out_wav.exists(): missing.append("output")
                if not mic_wav.exists(): missing.append("mic")
                if not ref_wav.exists(): missing.append("ref")
                print(f"WARN: AECMOS skipped on {cid} (missing: {','.join(missing)})",
                      file=sys.stderr)

            row["dnsmos_sig"]   = f"{dn_sig:.3f}"   if not math.isnan(dn_sig)   else ""
            row["dnsmos_bak"]   = f"{dn_bak:.3f}"   if not math.isnan(dn_bak)   else ""
            row["dnsmos_ovrl"]  = f"{dn_ovrl:.3f}"  if not math.isnan(dn_ovrl)  else ""
            row["aecmos_echo"]  = f"{ae_echo:.3f}"  if not math.isnan(ae_echo)  else ""
            row["aecmos_other"] = f"{ae_other:.3f}" if not math.isnan(ae_other) else ""
            row["aecmos_dt"]    = f"{ae_dt:.3f}"    if not math.isnan(ae_dt)    else ""
            writer.writerow(row)
    print(f"wrote {args.out_csv}: {n_rows} rows, DNSMOS scored {n_scored_dnsmos}, "
          f"AECMOS scored {n_scored_aecmos}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
