#!/usr/bin/env python3
"""Generate synthetic conditions for ecnr_eval (ADR-0011).

Produces the 3-track condition layout expected by `ecnr_eval --run`:

  conditions/synthetic/<case_id>/
    mic.wav             # what the chain sees: echo + (optional) near-end + (optional) noise
    ref.wav             # far-end render (AEC reference)
    echo_only_mic.wav   # echo as it arrives at the mic, no near-end, no noise
                        # (the true-ERLE oracle, per ADR-0011 §1)

The synthetic conditions exist for CI smoke and harness self-validation,
NOT for actual AEC3 tuning — synthetic echo paths are trivially canceller-
friendly and don't generalise to real cabins. The 134-case Phase 2 corpus
replaces these for real tuning work.

Echo path model: a 6-tap exponential decay (linear, time-invariant).
   echo[n] = sum_{k=0..5} 0.5^k * ref[n - 10*k]
Total length 50 samples ≈ 3 ms at 16 kHz; well within AEC3's adaptive
filter window. Easy case; AEC3 should learn this in a few hundred ms.

Run from the repo root:
   python3 reference/gen_eval_conditions.py
This writes (or overwrites) the committed fixtures under conditions/synthetic/.
No external dependencies beyond numpy + stdlib `wave`.
"""

from __future__ import annotations

import argparse
import pathlib
import wave

import numpy as np  # type: ignore

SR = 16000  # 16 kHz fixed; ADR-0003 baseline
DURATION_S = 5.0
N_SAMPLES = int(SR * DURATION_S)

# Echo path: 6 exponential taps spaced 10 samples apart.
# (See module docstring for the model rationale.)
ECHO_TAP_GAINS = [0.5 ** k for k in range(6)]
ECHO_TAP_DELAYS = [10 * k for k in range(6)]


def apply_echo_path(ref: np.ndarray) -> np.ndarray:
    """Convolve ref with the 6-tap echo path. Output is same length as ref."""
    out = np.zeros_like(ref)
    for gain, delay in zip(ECHO_TAP_GAINS, ECHO_TAP_DELAYS):
        if delay == 0:
            out += gain * ref
        else:
            out[delay:] += gain * ref[:-delay]
    # Scale so peak loudness is well below saturation; AEC3's tracker
    # behaves better when far-end and echo-at-mic are both ~-12 dBFS.
    return 0.5 * out


def synth_ref(seed: int) -> np.ndarray:
    """Synthesise a far-end render signal: a band-limited noise burst.

    Speech-shaped (LP-filtered) Gaussian noise stresses AEC3's adaptive
    filter on a wider spectral range than a single sine, while staying
    deterministic across runs (fixed seed) and synthesis-free of any
    third-party speech samples (which would introduce licence concerns
    for a committed fixture).
    """
    rng = np.random.default_rng(seed)
    raw = rng.standard_normal(N_SAMPLES)
    # 8-tap moving-average lowpass; cuts above ~2 kHz. Cheap and good
    # enough to make the signal "voice-like" in spectral envelope.
    kernel = np.ones(8) / 8.0
    filtered = np.convolve(raw, kernel, mode="same")
    # Normalise to -12 dBFS RMS.
    target_dbfs = -12.0
    rms = float(np.sqrt(np.mean(filtered ** 2)) + 1e-12)
    target_lin = 10.0 ** (target_dbfs / 20.0)
    return filtered * (target_lin / rms)


def add_noise(x: np.ndarray, target_dbfs: float, seed: int) -> np.ndarray:
    """Add white Gaussian noise at `target_dbfs` RMS to x."""
    rng = np.random.default_rng(seed)
    n = rng.standard_normal(len(x))
    target_lin = 10.0 ** (target_dbfs / 20.0)
    rms = float(np.sqrt(np.mean(n ** 2)) + 1e-12)
    n = n * (target_lin / rms)
    return x + n


def write_wav_mono_16k(path: pathlib.Path, samples: np.ndarray) -> None:
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767).astype(np.int16)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def write_case(out_root: pathlib.Path, case_id: str,
               mic: np.ndarray, ref: np.ndarray,
               echo_only_mic: np.ndarray) -> None:
    case_dir = out_root / case_id
    write_wav_mono_16k(case_dir / "ref.wav", ref)
    write_wav_mono_16k(case_dir / "echo_only_mic.wav", echo_only_mic)
    write_wav_mono_16k(case_dir / "mic.wav", mic)


def generate_case_001_quiet_cabin(out_root: pathlib.Path) -> None:
    """case_001: quiet cabin — echo only, no near-end, no noise.

    Easiest possible case. AEC3 should converge in <1 s; true ERLE
    expected ~30-60 dB after settle.
    """
    ref = synth_ref(seed=1)
    echo = apply_echo_path(ref)
    write_case(out_root, "case_001_quiet_cabin",
               mic=echo, ref=ref, echo_only_mic=echo)


def generate_case_002_with_noise(out_root: pathlib.Path) -> None:
    """case_002: echo + low-level white noise in the mic stream.

    Mic = echo + noise; echo_only_mic is the clean echo only (no noise).
    The harness should still report healthy true ERLE because true ERLE
    is computed against the clean echo input. Reported ERLE may be
    slightly worse because AEC3 sees the noisy mic.
    """
    ref = synth_ref(seed=2)
    echo = apply_echo_path(ref)
    mic_with_noise = add_noise(echo, target_dbfs=-40.0, seed=42)
    write_case(out_root, "case_002_with_noise",
               mic=mic_with_noise, ref=ref, echo_only_mic=echo)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-root", default="conditions/synthetic",
                    help="Output directory for the per-case subdirectories.")
    args = ap.parse_args()

    out_root = pathlib.Path(args.out_root)
    generate_case_001_quiet_cabin(out_root)
    generate_case_002_with_noise(out_root)
    print(f"Generated synthetic conditions under {out_root}/:")
    for case in sorted(out_root.iterdir()):
        if case.is_dir():
            wavs = sorted(case.glob("*.wav"))
            sizes = ", ".join(f"{w.name}={w.stat().st_size}B" for w in wavs)
            print(f"  {case.name}/  ({sizes})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
