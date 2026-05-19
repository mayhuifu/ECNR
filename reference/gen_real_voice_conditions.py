#!/usr/bin/env python3
"""Build the 'real_voice' conditions tree used by docs/phase-1-acceptance-grade.md.

Produces conditions/real_voice/<case_id>/ each containing:

  mic.wav            # real-voice + cabin-IR-echoed-caller + (optional) noise
  ref.wav            # the far-end render signal that produced the echo
  echo_only_mic.wav  # synthesized echo-only counterpart for the true-ERLE
                       oracle (cabin_ir convolution of ref). An APPROXIMATION —
                       real cabin recordings will replace this with a true
                       silence-pass per the phase-2-cabin-recording-protocol.

This sibling of gen_eval_conditions.py exists because the Phase-1 acceptance
grade needs *real-voice* content to produce meaningful DNSMOS scores. The
purely synthetic gen_eval_conditions.py output has no near-end voice and
DNSMOS correctly scores it as "no good speech in this clip" — uninformative.

The conditions/real_voice/ tree is gitignored (.gitignore *.wav rule); re-run
this script to regenerate. Source fixtures live at the paths below and are
fetched + pinned via reference/MANIFEST.tsv.

No external deps beyond numpy + scipy.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np  # type: ignore
from scipy.io import wavfile  # type: ignore
from scipy.signal import fftconvolve  # type: ignore


FIXTURES = [
    # (case_id, mic_path, ref_path, ir_path, near_end_clean_path_or_None)
    (
        "road_voice_realsynth",
        "reference/synth/test_mic_road.wav",
        "reference/synth/ref.wav",
        "reference/synth/cabin_ir.wav",
        None,  # test_mic_road already mixes voice; we don't have a separate clean track committed
    ),
    (
        "mixed_loud_echo",
        "reference/mixed_sound.wav",
        "reference/reference_sound_to_be_eliminated.wav",
        "reference/synth/cabin_ir.wav",
        None,
    ),
]


def read_mono_16k(p: str | Path) -> np.ndarray:
    fs, x = wavfile.read(str(p))
    if x.ndim == 2:
        # Downmix stereo to mono in the same dtype to preserve the int16 range.
        x = x.mean(axis=1).astype(x.dtype)
    if fs != 16000:
        raise RuntimeError(f"{p}: rate {fs}, expected 16000")
    return x


def write_16k(p: Path, x: np.ndarray) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    x = np.clip(x, -32768, 32767).astype(np.int16)
    wavfile.write(p, 16000, x)


def synth_echo_only(ref: np.ndarray, ir: np.ndarray, n: int,
                    peak_dbfs: float = -8.0) -> np.ndarray:
    """Convolve `ref` with `ir`, scale so peak amplitude is ~peak_dbfs."""
    ir_f = ir.astype(np.float32) / 32768.0
    echo = fftconvolve(ref.astype(np.float32), ir_f, mode="full")[:n]
    pk = max(1.0, float(np.max(np.abs(echo))))
    target_peak = 32767.0 * (10.0 ** (peak_dbfs / 20.0))
    return echo / pk * target_peak


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-root", default="conditions/real_voice", type=Path,
                    help="root directory for the per-case subdirectories")
    args = ap.parse_args()

    args.out_root.mkdir(parents=True, exist_ok=True)
    for case_id, mic_p, ref_p, ir_p, _ne_p in FIXTURES:
        cdir = args.out_root / case_id
        mic = read_mono_16k(mic_p)
        ref = read_mono_16k(ref_p)
        ir = read_mono_16k(ir_p)
        n = min(len(mic), len(ref))
        mic, ref = mic[:n], ref[:n]
        echo = synth_echo_only(ref, ir, n)
        write_16k(cdir / "mic.wav", mic)
        write_16k(cdir / "ref.wav", ref)
        write_16k(cdir / "echo_only_mic.wav", echo)
        print(f"  {case_id}: {n} samples ({n/16000:.2f} s) at 16 kHz")
    print(f"\nWrote {len(FIXTURES)} real-voice conditions under {args.out_root}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
