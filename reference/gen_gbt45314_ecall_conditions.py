#!/usr/bin/env python3
"""Generate GB/T 45314-2025 emergency-call pre-compliance fixtures.

This script intentionally uses only the Python standard library so the China
eCall release gate can run on the same minimal host used for build smoke tests.
The generated WAVs are deterministic and gitignored.

Real-recording mode (default when datasets/vehicle_noise/ is populated via
reference/fetch_vehicle_noise.py; --synthetic forces the legacy fixtures):

* far-end + near-end speech  <- ITU-T P.501-lineage fullband talkers
  (microsoft/P.808 mirror; GB/T 45314 normatively references P.501-2020,
  so these are the closest scriptable match to the standard's stimuli),
  polyphase-decimated 48 k -> 16 k with a windowed-sinc LPF.
* B1 road-noise scene        <- DEMAND TCAR channel 1 (real in-car driving
  noise, CC BY 4.0), deterministic segment offsets. Scene-mapping note:
  TCAR approximates the GB/T Annex A cruise scenes; the standard's own
  noise database (CATARC, 70+ vehicles) is not publicly downloadable.
* B2 HVAC scene              stays synthetic — no open-licensed idle+HVAC
  recording exists (research memo 2026-07-04); labelled in meta.txt.
* echo paths / time-variance unchanged — GB/T 5.5.3-style controlled
  signals are meant to be deterministic, not recorded.

The RELATIVE level plan (echo/near/road/HVAC offsets from the far-end
reference) is identical between modes so the checker floors — all
relative-dB metrics — stay calibrated. Real mode applies one global
crest-factor headroom attenuation (real speech peaks ~20 dB above RMS;
the flat synthetic proxy doesn't), recorded in meta.txt.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import random
import sys
import wave


SR = 16000
DURATION_S = 6.0
N = int(SR * DURATION_S)
FULL_SCALE = 32767.0

VEHICLE_NOISE_DIR = pathlib.Path(__file__).resolve().parent.parent / "datasets" / "vehicle_noise"
# Distinct talkers: far-end female (i01_f1), near-end male (i01_m1) — the
# double-talk proxy needs uncorrelated material on the two sides.
P501_FAREND = VEHICLE_NOISE_DIR / "p501" / "i01_f1.wav"
P501_NEAREND = VEHICLE_NOISE_DIR / "p501" / "i01_m1.wav"
TCAR_WAV = VEHICLE_NOISE_DIR / "tcar_ch01_16k.wav"
# Deterministic segment into the 300 s TCAR recording (skip the first
# minute: door/parking transients at the head of the take).
TCAR_OFFSET_S = 60.0


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


def read_wav_mono(path: pathlib.Path) -> tuple[list[float], int]:
    """16-bit PCM WAV -> float list in [-1, 1] (first channel) + sample rate."""
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: expected 16-bit PCM, got sampwidth={w.getsampwidth()}")
        n_ch = w.getnchannels()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    out: list[float] = []
    step = 2 * n_ch
    for i in range(0, len(raw) - step + 1, step):
        out.append(int.from_bytes(raw[i:i + 2], "little", signed=True) / FULL_SCALE)
    return out, sr


def decimate_48k_to_16k(x: list[float]) -> list[float]:
    """48 k -> 16 k via windowed-sinc LPF (cutoff ~7.2 kHz) + take-every-3rd.

    Pure stdlib and deterministic. 33 taps is plenty for a proxy fixture:
    stopband alias products land > 40 dB down, far below the level/
    correlation resolution the gate metrics use.
    """
    taps = 33
    half = taps // 2
    fc = 7200.0 / 48000.0  # normalized cutoff
    kernel: list[float] = []
    for k in range(taps):
        m = k - half
        s = 2.0 * fc if m == 0 else math.sin(2.0 * math.pi * fc * m) / (math.pi * m)
        # Hamming window
        s *= 0.54 - 0.46 * math.cos(2.0 * math.pi * k / (taps - 1))
        kernel.append(s)
    out: list[float] = []
    for center in range(0, len(x), 3):
        acc = 0.0
        for k in range(taps):
            idx = center + k - half
            if 0 <= idx < len(x):
                acc += x[idx] * kernel[k]
        out.append(acc)
    return out


def load_speech_16k(path: pathlib.Path, dbfs: float) -> list[float]:
    """Speech-active, pause-compressed 6 s excerpt at the target level.

    The convergence/TCL/double-talk clauses assume CONTINUOUS excitation —
    GB/T 45314's normative stimuli are ITU-T P.501 composite source
    signals, which are continuous by construction. Natural takes carry a
    silent lead-in and inter-sentence pauses; fed raw, they zero out the
    0-200 ms convergence window (no echo to cancel at t=0) and make the
    per-frame ERLE timeline swing wildly across far-end pauses. So:
    energy-gate 10 ms frames against (active level − 25 dB) and
    concatenate the speech-active frames — a CSS-style continuous signal
    with real spectro-temporal fine structure.
    """
    x, sr = read_wav_mono(path)
    if sr == 48000:
        x = decimate_48k_to_16k(x)
    elif sr != SR:
        sys.exit(f"{path}: unsupported rate {sr}")
    frame = SR // 100  # 10 ms
    frames = [x[i:i + frame] for i in range(0, len(x) - frame + 1, frame)]
    frame_rms = [rms(f) for f in frames]
    active = sorted(frame_rms)[int(0.95 * (len(frame_rms) - 1))]  # p95 ≈ speech level
    gate = active * (10.0 ** (-25.0 / 20.0))
    kept: list[float] = []
    for f, r in zip(frames, frame_rms):
        if r >= gate:
            kept.extend(f)
    if not kept:
        sys.exit(f"{path}: energy gate kept nothing — unexpected silence-only take")
    while len(kept) < N:  # loop-pad deterministically if the take is short
        kept.extend(kept[: N - len(kept)])
    return normalize_dbfs(kept[:N], dbfs)


def load_noise_segment_16k(path: pathlib.Path, offset_s: float, dbfs: float) -> list[float]:
    x, sr = read_wav_mono(path)
    if sr != SR:
        sys.exit(f"{path}: expected 16 kHz, got {sr}")
    start = int(offset_s * SR)
    if start + N > len(x):
        start = max(0, len(x) - N)
    return normalize_dbfs(x[start:start + N], dbfs)


def real_assets_present() -> bool:
    return P501_FAREND.exists() and P501_NEAREND.exists() and TCAR_WAV.exists()


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


def time_varying_echo(ref: list[float], echo_dbfs: float = -12.0) -> list[float]:
    echo_a = apply_echo_path(ref, "normal", echo_dbfs)
    echo_b = apply_echo_path(ref, "shifted", echo_dbfs)
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
    ap.add_argument("--synthetic", action="store_true",
                    help="force the legacy fully-synthetic fixtures even when "
                         "datasets/vehicle_noise/ real recordings are present")
    args = ap.parse_args()

    root = args.out_root
    use_real = real_assets_present() and not args.synthetic
    # Base level plan (dBFS RMS). All checker metrics are relative-dB, so
    # what must hold across modes is the RELATIVE plan: echo 2 dB above
    # far-end ref, near-end driver 6 dB below ref, road 10 dB below ref,
    # HVAC 12 dB below ref.
    ref_db, echo_db, near_db, road_db, hvac_db = -14.0, -12.0, -20.0, -24.0, -26.0
    headroom_off = 0.0
    if use_real:
        ref = load_speech_16k(P501_FAREND, ref_db)
        near = load_speech_16k(P501_NEAREND, near_db)
        # Real speech has ~20 dB crest factor vs the flat synthetic proxy —
        # at -14 dBFS RMS the P.501 takes clip (peak 2.4 FS). Apply ONE
        # global attenuation so the loudest signal peaks at <= 0.98 FS:
        # every relative relationship above is preserved exactly.
        peak = max(max(abs(v) for v in ref), max(abs(v) for v in near))
        if peak > 0.98:
            headroom_off = 20.0 * math.log10(peak / 0.98)
            ref_db -= headroom_off
            echo_db -= headroom_off
            near_db -= headroom_off
            road_db -= headroom_off
            hvac_db -= headroom_off
            ref = normalize_dbfs(ref, ref_db)
            near = normalize_dbfs(near, near_db)
        road = load_noise_segment_16k(TCAR_WAV, TCAR_OFFSET_S, road_db)
        speech_src = ("ITU-T P.501-lineage (microsoft/P.808 3gpp_p501_FB; far=i01_f1, "
                      "near=i01_m1; 48k->16k sinc decimation)")
        road_src = (f"DEMAND TCAR ch01 (CC BY 4.0) segment @ {TCAR_OFFSET_S:.0f}s; "
                    "approximates GB/T Annex A cruise scenes (CATARC DB not public)")
        print(f"mode: REAL recordings (P.501-lineage speech + DEMAND TCAR road noise); "
              f"crest-factor headroom offset -{headroom_off:.1f} dB")
    else:
        if not args.synthetic:
            print("WARNING: datasets/vehicle_noise/ not populated — falling back to "
                  "synthetic fixtures. Run reference/fetch_vehicle_noise.py first "
                  "for the real-recording conditions.", file=sys.stderr)
        ref = speech_like(100, ref_db)
        near = speech_like(400, near_db)
        road = road_noise(200, road_db)
        speech_src = "synthetic voiced-speech proxy"
        road_src = "synthetic road-noise proxy"
        print("mode: SYNTHETIC fixtures")
    echo = apply_echo_path(ref, "normal", echo_db)
    # B2 HVAC stays synthetic in both modes: no open-licensed idle+HVAC
    # cabin recording exists (2026-07-04 research memo).
    hvac = hvac_noise(300, hvac_db)
    zero = [0.0 for _ in range(N)]
    prov = (f"speech: {speech_src}; road noise: {road_src}; levels (dBFS RMS): "
            f"ref {ref_db:.1f} / echo {echo_db:.1f} / near {near_db:.1f} / road {road_db:.1f} "
            f"/ hvac {hvac_db:.1f} (relative plan fixed; global headroom offset "
            f"{headroom_off:.1f} dB in real mode)")

    write_case(
        root,
        "ecall_farend_quiet_tcl_convergence",
        mic=echo,
        ref=ref,
        echo_only=echo,
        notes="GB/T 45314 5.5 proxy: quiet far-end single-talk TCL and initial convergence. " + prov,
    )
    write_case(
        root,
        "ecall_farend_b1_road_convergence",
        mic=add(echo, road),
        ref=ref,
        echo_only=echo,
        notes="GB/T 45314 Annex A B1 proxy: far-end single-talk with road noise. " + prov,
    )
    tv_echo = time_varying_echo(ref, echo_db)
    write_case(
        root,
        "ecall_timevarying_path",
        mic=tv_echo,
        ref=ref,
        echo_only=tv_echo,
        notes="GB/T 45314 5.5.3 proxy: time-varying echo path, method-c style abrupt cabin-path change. " + prov,
    )
    write_case(
        root,
        "ecall_doubletalk_driver_minus6",
        mic=add(echo, near),
        ref=ref,
        echo_only=echo,
        near=near,
        notes="GB/T 45314 5.7 proxy: double-talk with near-end driver speech 6 dB below nominal far-end. " + prov,
    )
    write_case(
        root,
        "ecall_b2_noise_only_stability",
        mic=hvac,
        ref=zero,
        echo_only=zero,
        notes="GB/T 45314 5.8.1 / Annex A B2 proxy: no-speech HVAC-like noise stability (synthetic in both modes: no open idle+HVAC recording). " + prov,
    )

    print(f"Generated GB/T 45314 eCall conditions under {root}/")
    for case in sorted(root.iterdir()):
        if case.is_dir():
            print(f"  {case.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
