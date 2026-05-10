#!/usr/bin/env python3
"""Generate a synthetic 16 kHz mic/ref pair for the Phase 0 bench harness.

ref.wav   = white noise (the "far-end" stimulus, what would play out the speaker)
mic.wav   = ref convolved with a short synthetic IR + low-amplitude near-end speech-like tone
            (stand-in for "speech under echo"; not real speech)

The pair is time-aligned at sample 0; AEC's delay estimator handles the rest.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import struct
import wave

import numpy as np  # type: ignore

SR = 16000


def write_wav(path: pathlib.Path, samples: np.ndarray) -> None:
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--duration", type=float, default=10.0)
    p.add_argument("--out-dir", default="reference/synth")
    args = p.parse_args()

    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    n = int(args.duration * SR)
    rng = np.random.default_rng(0xECA0)

    # Far-end: white noise at -12 dBFS
    ref = rng.standard_normal(n) * 0.25

    # Synthetic 30 ms exponential-decay IR (cabin-ish but tiny)
    ir_len = int(0.030 * SR)
    ir = rng.standard_normal(ir_len) * np.exp(-np.arange(ir_len) / (0.005 * SR))
    ir /= np.linalg.norm(ir) + 1e-9
    ir *= 0.5  # echo level

    echo = np.convolve(ref, ir)[:n]

    # Near-end "speech": 220 Hz tone with envelope, ducked while echo is loud
    t = np.arange(n) / SR
    near = 0.15 * np.sin(2 * math.pi * 220 * t) * (0.5 + 0.5 * np.sin(2 * math.pi * 0.5 * t))

    mic = echo + near

    write_wav(out / "ref.wav", ref)
    write_wav(out / "mic.wav", mic)
    write_wav(out / "near_clean.wav", near)
    print(f"wrote {out}/{{ref,mic,near_clean}}.wav  ({args.duration:.1f}s @ {SR} Hz)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
