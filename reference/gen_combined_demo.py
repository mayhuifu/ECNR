#!/usr/bin/env python3
"""Compose a 60-second canned AEC + NS demo with realistic background noises.

This is the no-human-in-the-loop counterpart to `ecnr_live`. It produces:

  reference/synth/demo_60s_ref.wav  — the far-end stimulus (caller voice
                                       looped, what the speaker would play)
  reference/synth/demo_60s_mic.wav  — the synthesized mic capture: near-end
                                       voice + cabin-IR'd echo + background
                                       noises layered per scenario

A 60-second walkthrough lets a single A/B (`afplay demo_60s_mic.wav` then
`afplay /tmp/demo_60s_after.wav` after running ecnr_bench) demonstrate AEC
+ NS behaviour across multiple realistic environments without requiring
the user to speak live or play stimuli through real speakers.

Scene timeline (default):

   0–10 s  quiet baseline                (cabin echo only)
  10–20 s  + car interior                (real recording — road / engine rumble)
  20–30 s  + cafe babble                 (real recording — multi-talker speech)
  30–40 s  + music                       (real recording — Schumann, classical)
  40–50 s  + stadium crowd               (real recording — football crowd Oehh)
  50–60 s  + dog bark transients         (real recording — sparse barks)

Noise files come from reference/noise/ (real recordings, fetched via
scripts/fetch-noise.sh). For any file that's missing, a synthetic fallback
from reference/synth/ is used so the demo always runs end-to-end.

Audio decode is delegated to ffmpeg (handles .wav / .ogg / .mp3 / .flac and
auto-resamples to 16 kHz mono). Run --print-timeline to see what would happen
without writing any file.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import subprocess
import sys
import wave

import numpy as np  # type: ignore

SR = 16000
DURATION_S = 60.0
N_TOTAL = int(DURATION_S * SR)

# Path map. `path` is the preferred file; `fallback` is the synthetic version.
# Either may be absent; the composer uses whichever exists, and skips the scene
# if neither is present (with a clear log line).
SOURCES = {
    "near_voice": {
        "path": "reference/synth/voice_synth.wav",
        "fallback": None,  # required — no synthetic fallback
        "purpose": "near-end speech (the user, looped throughout)",
    },
    "ref_voice": {
        "path": "reference/synth/ref_voice.wav",
        "fallback": None,  # required
        "purpose": "far-end stimulus (caller voice through speaker, looped throughout)",
    },
    "cabin_ir": {
        "path": "reference/synth/cabin_ir.wav",
        "fallback": None,  # required
        "purpose": "speaker→mic acoustic IR",
    },
    "car_interior": {
        "path": "reference/noise/car_interior.wav",
        "fallback": "reference/synth/noise_road.wav",
        "purpose": "scene noise: road / engine rumble",
    },
    "cafe_babble": {
        "path": "reference/noise/cafe_babble.wav",
        "fallback": None,  # synthetic babble doesn't exist; scene skipped if missing
        "purpose": "scene noise: multi-talker babble",
    },
    "music": {
        "path": "reference/noise/music.ogg",
        "fallback": "reference/synth/ref_music.wav",
        "purpose": "scene noise: background music",
    },
    "stadium_crowd": {
        "path": "reference/noise/stadium_crowd.wav",
        "fallback": None,  # synthetic stadium doesn't exist; skipped if missing
        "purpose": "scene noise: stadium crowd",
    },
    "dog_bark": {
        "path": "reference/noise/dog_bark.wav",
        "fallback": "reference/synth/noise_bark.wav",
        "purpose": "scene noise: dog bark transient",
    },
}

# Scene list: (start_s, duration_s, name, source_key, scene_gain_db)
# scene_gain_db is applied on top of RMS-normalized clips (-20 dBFS RMS each)
# so a single number consistently means "noise level in the mic mix relative
# to a -20 dBFS reference." Numbers chosen so a sum of two overlapping scenes
# stays under full-scale with the near-end + echo also present.
SCENES = [
    (10.0, 10.0, "car interior",  "car_interior",  -8.0),
    (20.0, 10.0, "cafe babble",   "cafe_babble",   -10.0),
    (30.0, 10.0, "music",         "music",         -14.0),
    (40.0, 10.0, "stadium crowd", "stadium_crowd", -10.0),
    # Dog barks: two well-separated transients in the last segment.
    (50.0,  3.0, "dog bark #1",   "dog_bark",      -3.0),
    (55.0,  3.0, "dog bark #2",   "dog_bark",      -3.0),
]

# Echo gain: matches gen_test_input.py default for consistency. The cabin IR's
# L2 normalization keeps post-convolution energy comparable across IRs.
ECHO_GAIN = 0.4
NEAR_GAIN_DB = 0.0  # near-end is RMS-normalized to -18 dBFS at source already


def load_via_ffmpeg(path: pathlib.Path) -> np.ndarray:
    """Decode any audio file to 16 kHz mono float64 in [-1, 1] via ffmpeg.

    Avoids requiring scipy / pydub / soundfile — ffmpeg is already a project
    build dep (used to construct cabin_ir.wav etc.) and handles every format
    we care about.
    """
    proc = subprocess.run(
        [
            "ffmpeg", "-nostdin", "-loglevel", "error",
            "-i", str(path),
            "-ar", str(SR), "-ac", "1",
            "-f", "f32le", "-",
        ],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return np.frombuffer(proc.stdout, dtype=np.float32).astype(np.float64)


def write_wav_mono_16k(path: pathlib.Path, samples: np.ndarray) -> None:
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def loop_or_clip(samples: np.ndarray, target_n: int) -> np.ndarray:
    """Tile or clip samples to exactly target_n samples."""
    if len(samples) >= target_n:
        return samples[:target_n].copy()
    if len(samples) == 0:
        return np.zeros(target_n, dtype=np.float64)
    reps = (target_n + len(samples) - 1) // len(samples)
    return np.tile(samples, reps)[:target_n].copy()


def normalize_rms(x: np.ndarray, target_dbfs: float) -> np.ndarray:
    rms = float(np.sqrt(np.mean(x * x)) + 1e-12)
    if rms < 1e-9:
        return x  # silence — leave alone
    target = 10.0 ** (target_dbfs / 20.0)
    return x * (target / rms)


def fade_in_out(samples: np.ndarray, fade_n: int) -> np.ndarray:
    """Apply cosine fade-in and fade-out of fade_n samples each (in-place safe)."""
    fade_n = min(fade_n, len(samples) // 2)
    if fade_n <= 0:
        return samples
    out = samples.copy()
    ramp = 0.5 * (1.0 - np.cos(np.linspace(0.0, math.pi, fade_n)))
    out[:fade_n] *= ramp
    out[-fade_n:] *= ramp[::-1]
    return out


def resolve_source(key: str, sources_root: pathlib.Path) -> tuple[pathlib.Path | None, str]:
    """Return (path, kind) for a source key. kind is 'real', 'fallback', or 'missing'."""
    info = SOURCES[key]
    primary = sources_root / info["path"]
    if primary.exists():
        return primary, "real"
    if info["fallback"]:
        fb = sources_root / info["fallback"]
        if fb.exists():
            return fb, "fallback"
    return None, "missing"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    p.add_argument(
        "--out-dir",
        default="reference/synth",
        help="output directory for demo_60s_*.wav (default: reference/synth)",
    )
    p.add_argument(
        "--root",
        default=".",
        help="project root for resolving reference/synth and reference/noise (default: .)",
    )
    p.add_argument(
        "--print-timeline",
        action="store_true",
        help="print the scene resolution table and exit; don't write any output",
    )
    args = p.parse_args()

    root = pathlib.Path(args.root).resolve()
    out_dir = pathlib.Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = root / out_dir

    # Resolve every source up front so the timeline log is honest.
    print(f"# composing 60s demo from sources rooted at {root}")
    print(f"#   output: {out_dir}/demo_60s_{{ref,mic}}.wav")
    print()
    print(f"# {'key':<14} {'kind':<8} {'path'}")
    resolved: dict[str, tuple[pathlib.Path | None, str]] = {}
    for key in SOURCES:
        path, kind = resolve_source(key, root)
        resolved[key] = (path, kind)
        marker = "✓" if kind == "real" else ("~" if kind == "fallback" else "✗")
        path_str = str(path.relative_to(root)) if path else "(missing)"
        print(f"# {marker} {key:<14} {kind:<8} {path_str}")
    print()

    # Required sources present?
    for key in ("near_voice", "ref_voice", "cabin_ir"):
        if resolved[key][0] is None:
            print(f"error: required source '{key}' is missing — "
                  f"run `python3 reference/gen_synth.py --duration 10 --out-dir reference/synth/` "
                  f"to regenerate the synthetic baseline files.", file=sys.stderr)
            return 2

    if args.print_timeline:
        print("# scene timeline (would be applied):")
        for start_s, dur_s, name, key, gain_db in SCENES:
            path, kind = resolved[key]
            tag = "real" if kind == "real" else ("synthetic fallback" if kind == "fallback" else "SKIP — missing")
            print(f"#   {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name:<16}  gain={gain_db:+5.1f} dB  [{tag}]")
        return 0

    # ------------------------- Build the ref stream -------------------------
    ref_path, _ = resolved["ref_voice"]
    assert ref_path is not None
    ref_src = load_via_ffmpeg(ref_path)
    ref_full = loop_or_clip(ref_src, N_TOTAL)
    write_wav_mono_16k(out_dir / "demo_60s_ref.wav", ref_full)

    # ------------------------- Build the mic stream -------------------------
    # 1. Near-end voice (looped voice_synth) — what AEC must preserve
    near_path, _ = resolved["near_voice"]
    assert near_path is not None
    near_src = load_via_ffmpeg(near_path)
    near_full = loop_or_clip(near_src, N_TOTAL)
    near_gain = 10.0 ** (NEAR_GAIN_DB / 20.0)

    # 2. Echo path: ref convolved with cabin IR, scaled
    cabin_ir_path, _ = resolved["cabin_ir"]
    assert cabin_ir_path is not None
    cabin_ir = load_via_ffmpeg(cabin_ir_path)
    echo_full = np.convolve(ref_full, cabin_ir)[:N_TOTAL] * ECHO_GAIN

    mic = near_gain * near_full + echo_full

    # 3. Per-scene noise overlays
    print("# applying scenes:")
    for start_s, dur_s, name, key, gain_db in SCENES:
        path, kind = resolved[key]
        if path is None:
            print(f"  SKIP    {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name}  (file missing)")
            continue

        scene_n = int(dur_s * SR)
        start_n = int(start_s * SR)
        if start_n + scene_n > N_TOTAL:
            scene_n = N_TOTAL - start_n
            if scene_n <= 0:
                continue

        clip_src = load_via_ffmpeg(path)
        if len(clip_src) == 0:
            print(f"  SKIP    {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name}  (empty clip)")
            continue

        # Normalize each clip to -20 dBFS RMS so scene gain is comparable
        # across sources (without normalization, music's peak vs babble's
        # peak vs road's peak would all mean different things).
        clip_normalized = normalize_rms(clip_src, target_dbfs=-20.0)
        clip_looped = loop_or_clip(clip_normalized, scene_n)

        # 200 ms cosine fade in/out at scene boundaries — avoids audible clicks
        # when the noise drops in and out.
        clip_faded = fade_in_out(clip_looped, fade_n=int(0.200 * SR))

        gain_lin = 10.0 ** (gain_db / 20.0)
        mic[start_n:start_n + scene_n] += gain_lin * clip_faded

        kind_tag = "real" if kind == "real" else "synth"
        print(f"  apply   {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name:<16}  "
              f"gain={gain_db:+5.1f} dB  [{kind_tag}]")

    # Final clip safety: if the sum exceeded full scale, scale the whole mic
    # stream by a single factor (preserves relative levels, avoids hard-clipping
    # which AEC3 cannot cancel). Better here than relying on int16 saturation.
    peak = float(np.max(np.abs(mic)))
    if peak > 0.99:
        scale = 0.99 / peak
        mic *= scale
        print(f"# headroom adjust: peak was {peak:.3f}, scaled by {scale:.3f}")

    write_wav_mono_16k(out_dir / "demo_60s_mic.wav", mic)
    print()
    print(f"wrote {out_dir}/demo_60s_ref.wav + demo_60s_mic.wav  ({DURATION_S:.0f}s @ {SR // 1000} kHz mono)")
    print()
    print("next: ./build/ecnr_bench \\")
    print(f"        --mic {out_dir}/demo_60s_mic.wav \\")
    print(f"        --ref {out_dir}/demo_60s_ref.wav \\")
    print("        --out /tmp/demo_60s_after.wav")
    print(f"      afplay {out_dir}/demo_60s_mic.wav   # before")
    print("      afplay /tmp/demo_60s_after.wav        # after")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
