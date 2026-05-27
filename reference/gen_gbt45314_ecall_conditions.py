#!/usr/bin/env python3
"""Generate GB/T 45314-2025 emergency-call pre-compliance fixtures.

This script intentionally uses only the Python standard library so the China
eCall release gate can run on the same minimal host used for build smoke tests.
The generated WAVs are deterministic and gitignored.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import random
import wave


SR = 16000
DURATION_S = 6.0
N = int(SR * DURATION_S)
FULL_SCALE = 32767.0


def rms(x: list[float]) -> float:
    if not x:
        return 0.0
    return math.sqrt(sum(v * v for v in x) / len(x)) + 1e-12


def normalize_dbfs(x: list[float], dbfs: float) -> list[float]:
    scale = (10.0 ** (dbfs / 20.0)) / rms(x)
    return [v * scale for v in x]


def moving_average(x: list[float], width: int) -> list[float]:
    out: list[float] = []
    acc = 0.0
    q: list[float] = []
    for v in x:
        q.append(v)
        acc += v
        if len(q) > width:
            acc -= q.pop(0)
        out.append(acc / len(q))
    return out


def speech_like(seed: int, dbfs: float) -> list[float]:
    rng = random.Random(seed)
    out: list[float] = []
    phase = 0.0
    f0 = 145.0 + (seed % 37)
    for i in range(N):
        t = i / SR
        f0_inst = f0 + 22.0 * math.sin(2.0 * math.pi * 0.65 * t)
        phase += 2.0 * math.pi * f0_inst / SR
        syllable = max(0.0, math.sin(2.0 * math.pi * 2.7 * t))
        envelope = 0.12 + 0.88 * syllable
        if int(t * 4.0 + seed) % 7 == 0:
            envelope *= 0.20
        sample = 0.0
        for harmonic in range(1, 18):
            freq = harmonic * f0_inst
            # Broad formant weighting around vowel-like bands. This is still
            # synthetic, but closer to voiced speech than filtered noise and
            # therefore a better RNNoise/double-talk preservation proxy.
            formant = (
                1.20 * math.exp(-((freq - 650.0) / 260.0) ** 2) +
                0.90 * math.exp(-((freq - 1250.0) / 360.0) ** 2) +
                0.45 * math.exp(-((freq - 2500.0) / 700.0) ** 2)
            )
            sample += (formant / harmonic) * math.sin(harmonic * phase)
        sample += 0.012 * rng.gauss(0.0, 1.0)
        out.append(sample * envelope)
    return normalize_dbfs(out, dbfs)


def road_noise(seed: int, dbfs: float) -> list[float]:
    rng = random.Random(seed)
    acc = 0.0
    out: list[float] = []
    for i in range(N):
        acc = 0.998 * acc + rng.gauss(0.0, 1.0)
        t = i / SR
        rumble = 0.25 * math.sin(2.0 * math.pi * 85.0 * t)
        wobble = 0.18 * math.sin(2.0 * math.pi * 6.0 * t)
        out.append(acc + rumble + wobble)
    return normalize_dbfs(out, dbfs)


def hvac_noise(seed: int, dbfs: float) -> list[float]:
    rng = random.Random(seed)
    raw = [rng.gauss(0.0, 1.0) for _ in range(N)]
    smooth = moving_average(raw, 64)
    out: list[float] = []
    for i, v in enumerate(smooth):
        t = i / SR
        hum = 0.30 * math.sin(2.0 * math.pi * 120.0 * t)
        fan = 0.18 * math.sin(2.0 * math.pi * 240.0 * t + 0.4)
        out.append(0.55 * v + hum + fan)
    return normalize_dbfs(out, dbfs)


def cabin_path(kind: str) -> tuple[list[tuple[int, float]], float]:
    if kind == "shifted":
        taps = [(120, 0.75), (430, 0.38), (1170, 0.18)]
        decay_gain = 0.20
    else:
        taps = [(45, 0.80), (260, 0.45), (850, 0.22)]
        decay_gain = 0.18
    return taps, decay_gain


def apply_echo_path(ref: list[float], kind: str, dbfs: float = -12.0) -> list[float]:
    taps, decay_gain = cabin_path(kind)
    out = [0.0 for _ in range(len(ref))]
    tail = 0.0
    tail_decay = math.exp(-1.0 / (0.045 * SR))
    for i in range(len(ref)):
        tail = tail * tail_decay + ref[i] * decay_gain
        y = tail
        for delay, gain in taps:
            if i >= delay:
                y += ref[i - delay] * gain
        out[i] = y
    return normalize_dbfs(out, dbfs)


def time_varying_echo(ref: list[float]) -> list[float]:
    echo_a = apply_echo_path(ref, "normal", -12.0)
    echo_b = apply_echo_path(ref, "shifted", -12.0)
    out = echo_a[:]
    switch = int(3.0 * SR)
    cross = int(0.080 * SR)
    for i in range(cross):
        a = i / max(1, cross - 1)
        out[switch + i] = (1.0 - a) * echo_a[switch + i] + a * echo_b[switch + i]
    out[switch + cross :] = echo_b[switch + cross :]
    return out


def add(*signals: list[float]) -> list[float]:
    return [sum(parts) for parts in zip(*signals)]


def write_wav(path: pathlib.Path, x: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = bytearray()
    for v in x:
        s = int(max(-0.999, min(0.999, v)) * FULL_SCALE)
        pcm += int(s).to_bytes(2, byteorder="little", signed=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(pcm))


def write_case(root: pathlib.Path, case_id: str, *, mic: list[float],
               ref: list[float], echo_only: list[float],
               near: list[float] | None = None, notes: str) -> None:
    cdir = root / case_id
    write_wav(cdir / "mic.wav", mic)
    write_wav(cdir / "ref.wav", ref)
    write_wav(cdir / "echo_only_mic.wav", echo_only)
    if near is not None:
        write_wav(cdir / "near_end_clean.wav", near)
    (cdir / "meta.txt").write_text(notes + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-root", default="conditions/gbt45314_ecall",
                    type=pathlib.Path)
    args = ap.parse_args()

    root = args.out_root
    ref = speech_like(100, -14.0)
    echo = apply_echo_path(ref, "normal", -12.0)
    road = road_noise(200, -24.0)
    hvac = hvac_noise(300, -26.0)
    near = speech_like(400, -20.0)
    zero = [0.0 for _ in range(N)]

    write_case(
        root,
        "ecall_farend_quiet_tcl_convergence",
        mic=echo,
        ref=ref,
        echo_only=echo,
        notes="GB/T 45314 5.5 proxy: quiet far-end single-talk TCL and initial convergence.",
    )
    write_case(
        root,
        "ecall_farend_b1_road_convergence",
        mic=add(echo, road),
        ref=ref,
        echo_only=echo,
        notes="GB/T 45314 Annex A B1 proxy: far-end single-talk with road noise.",
    )
    tv_echo = time_varying_echo(ref)
    write_case(
        root,
        "ecall_timevarying_path",
        mic=tv_echo,
        ref=ref,
        echo_only=tv_echo,
        notes="GB/T 45314 5.5.3 proxy: time-varying echo path, method-c style abrupt cabin-path change.",
    )
    write_case(
        root,
        "ecall_doubletalk_driver_minus6",
        mic=add(echo, near),
        ref=ref,
        echo_only=echo,
        near=near,
        notes="GB/T 45314 5.7 proxy: double-talk with near-end driver speech 6 dB below nominal far-end.",
    )
    write_case(
        root,
        "ecall_b2_noise_only_stability",
        mic=hvac,
        ref=zero,
        echo_only=zero,
        notes="GB/T 45314 5.8.1 / Annex A B2 proxy: no-speech HVAC-like noise stability.",
    )

    print(f"Generated GB/T 45314 eCall conditions under {root}/")
    for case in sorted(root.iterdir()):
        if case.is_dir():
            print(f"  {case.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
