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

# Two duration profiles. The long variant (60 s, 10 s scenes) is the headline
# walkthrough; the short variant (30 s, 5 s scenes) is the easier-to-A/B
# version for quick demos and parameter sweeps. Selected via --short.
LONG_DURATION_S = 60.0
SHORT_DURATION_S = 30.0

# Path map. `path` is the preferred file; `fallback` is the synthetic version.
# Either may be absent; the composer uses whichever exists, and skips the scene
# if neither is present (with a clear log line).
SOURCES = {
    "near_voice": {
        # Prefer a real recording (./build/ecnr_live --record-voice
        # reference/synth/voice_recorded.wav --duration 60). The fallback is
        # cafe_babble.wav — a real café/restaurant ambience that gives the
        # demo a "calling from a noisy environment" character. The synthetic
        # voice_synth.wav was the previous fallback but sounded robotic
        # enough to distract from the AEC/NS behaviour the demo exists to
        # showcase.
        "path": "reference/synth/voice_recorded.wav",
        "fallback": "reference/noise/cafe_babble.wav",
        "purpose": "near-end speech / environment (looped throughout)",
    },
    "ref_voice": {
        # Prefer a real recording (./build/ecnr_live --record-voice
        # reference/synth/caller_recorded.wav --duration 60) — the synthetic
        # ref_voice.wav is the fallback. Without a real recording the caller
        # voice can sound abstract, making AEC's removal harder to perceive.
        "path": "reference/synth/caller_recorded.wav",
        "fallback": "reference/synth/ref_voice.wav",
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
SCENES_LONG = [
    (10.0, 10.0, "car interior",  "car_interior",  -8.0),
    (20.0, 10.0, "cafe babble",   "cafe_babble",   -10.0),
    (30.0, 10.0, "music",         "music",         -14.0),
    (40.0, 10.0, "stadium crowd", "stadium_crowd", -10.0),
    # Dog barks: two well-separated transients in the last segment.
    (50.0,  3.0, "dog bark #1",   "dog_bark",      -3.0),
    (55.0,  3.0, "dog bark #2",   "dog_bark",      -3.0),
]

# Short timeline (30 s total). Same scene order, half the time per scene.
# Dog bark slot only fits one transient instead of two.
SCENES_SHORT = [
    ( 5.0, 5.0, "car interior",  "car_interior",  -8.0),
    (10.0, 5.0, "cafe babble",   "cafe_babble",   -10.0),
    (15.0, 5.0, "music",         "music",         -14.0),
    (20.0, 5.0, "stadium crowd", "stadium_crowd", -10.0),
    (25.0, 2.5, "dog bark",      "dog_bark",      -3.0),
]

# Speaker→mic acoustic path is modeled as TWO components added together:
#
#   1. A direct-coupling path — ref_voice mixed into the mic at a small fixed
#      delay (~2 ms, the speaker-to-nearest-mic flight time at ~0.7 m), no IR
#      convolution. This makes the caller voice clearly audible and recognizable
#      in the mic stream — without it, the caller is only present as smeared
#      reverberant energy via the cabin IR and the demo's "AEC removed the
#      caller" moment becomes perceptually subtle.
#
#   2. A reverberant echo path — ref_voice convolved with the cabin IR (scaled
#      by ECHO_GAIN). This is the cabin reflection tail.
#
# Both are linear transforms of ref_voice, so AEC3 sees them as a single
# composite IR and adapts to cancel their sum — same as a real speaker→mic
# path with both early direct arrivals and a reverberant tail.
DIRECT_COUPLING_GAIN_DB = -10.0
DIRECT_COUPLING_DELAY_MS = 2.0
ECHO_GAIN = 0.4
NEAR_GAIN_DB = 0.0
# Voice sources (near_voice + ref_voice) get loudness-normalized on load.
# Two cases differ a lot:
#   - Synthetic generators target -18 dBFS RMS, peak ~-3 dBFS (peak/RMS ~15 dB).
#   - Real recordings via `ecnr_live --record-voice` have natural speech
#     dynamics — peak/RMS often 20-30 dB, with silences dragging RMS down
#     even further (recordings at -40 dBFS RMS with peaks at -15 dBFS are
#     normal). Pure RMS normalization in this case would over-amplify the
#     peaks and hard-clip them, which AEC3 cannot cancel (clipping is a
#     non-linearity outside the linear filter's model).
# So: RMS-normalize up to the target, but cap the gain so peaks stay below
# VOICE_PEAK_CEILING_DBFS. Whichever constraint binds tighter wins.
VOICE_TARGET_RMS_DBFS = -18.0
VOICE_PEAK_CEILING_DBFS = -1.0


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


def active_speech_rms_dbfs(x: np.ndarray, active_thresh_dbfs: float = -50.0,
                           win_ms: int = 200) -> float:
    """RMS over above-threshold windows — perceived loudness of active speech.

    Plain whole-file RMS is dragged down by silences (typical of natural
    recordings: 50% active, RMS biased ~3 dB below active-speech RMS, more
    if there's a lot of silence). Active RMS targets the windows that
    actually contain speech, matching how broadcast loudness metrics
    (EBU R128) gate out silence. Falls back to plain RMS if everything
    is below threshold (e.g., the whole file is silent).
    """
    win_n = max(1, int(win_ms * SR // 1000))
    if len(x) < win_n:
        return 20.0 * math.log10(float(np.sqrt(np.mean(x * x)) + 1e-12))
    n_windows = len(x) // win_n
    truncated = x[:n_windows * win_n].reshape(n_windows, win_n)
    win_energy = np.mean(truncated * truncated, axis=1)
    win_db = 10.0 * np.log10(win_energy + 1e-24)
    active_mask = win_db > active_thresh_dbfs
    if not np.any(active_mask):
        return 20.0 * math.log10(float(np.sqrt(np.mean(x * x)) + 1e-12))
    active_energy = float(np.mean(win_energy[active_mask]))
    return 10.0 * math.log10(active_energy + 1e-24)


def normalize_voice(x: np.ndarray, target_rms_dbfs: float,
                    peak_ceiling_dbfs: float) -> tuple[np.ndarray, float, str]:
    """RMS-normalize x to target_rms_dbfs using ACTIVE-speech RMS (silence-gated),
    but cap gain so peak ≤ peak_ceiling.

    Returns (normalized_x, applied_gain_db, binding_constraint) where
    binding_constraint is 'rms' or 'peak'. Peak-binding means the source has
    natural dynamics (recording with strong peaks); rms-binding means the
    peak/RMS ratio is moderate (synthetic or already-compressed source).

    Using active-speech RMS as the loudness anchor matters for recordings:
    a 60-second recording with 30 seconds of silence has whole-file RMS
    ~3 dB below its active-speech RMS, more if the speech itself is sparse.
    Normalizing on whole-file RMS would over-amplify, then peak-cap, leaving
    the speech still 10+ dB below target. Active RMS sidesteps that.
    """
    active_rms_db = active_speech_rms_dbfs(x)
    peak = float(np.max(np.abs(x)) + 1e-12)
    if active_rms_db < -120.0 or peak < 1e-9:
        return x, 0.0, "rms"  # essentially silent

    rms_gain_db = target_rms_dbfs - active_rms_db
    peak_gain_db = peak_ceiling_dbfs - 20.0 * math.log10(peak)

    gain_db = min(rms_gain_db, peak_gain_db)
    constraint = "peak" if peak_gain_db < rms_gain_db else "rms"
    gain_lin = 10.0 ** (gain_db / 20.0)
    return x * gain_lin, gain_db, constraint


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
        help="output directory for demo_{30,60}s_*.wav (default: reference/synth)",
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
    p.add_argument(
        "--short",
        action="store_true",
        help=(
            "produce the 30-second variant (5-second scenes) — easier to A/B "
            "and quicker to listen through. Output files are written as "
            "demo_30s_*.wav and coexist with demo_60s_*.wav."
        ),
    )
    args = p.parse_args()

    # Profile selection. `out_prefix` is what discriminates the two variants
    # on disk so the long and short demos can coexist without overwriting
    # each other.
    duration_s = SHORT_DURATION_S if args.short else LONG_DURATION_S
    n_total = int(duration_s * SR)
    scenes = SCENES_SHORT if args.short else SCENES_LONG
    out_prefix = "demo_30s" if args.short else "demo_60s"

    root = pathlib.Path(args.root).resolve()
    out_dir = pathlib.Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = root / out_dir

    # Resolve every source up front so the timeline log is honest.
    print(f"# composing {duration_s:.0f}s demo from sources rooted at {root}")
    print(f"#   output: {out_dir}/{out_prefix}_{{ref,mic}}.wav")
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
        for start_s, dur_s, name, key, gain_db in scenes:
            path, kind = resolved[key]
            tag = "real" if kind == "real" else ("synthetic fallback" if kind == "fallback" else "SKIP — missing")
            print(f"#   {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name:<16}  gain={gain_db:+5.1f} dB  [{tag}]")
        return 0

    def _stats_db(x: np.ndarray) -> tuple[float, float, float]:
        rms = 20.0 * math.log10(float(np.sqrt(np.mean(x * x)) + 1e-12))
        peak = 20.0 * math.log10(float(np.max(np.abs(x)) + 1e-12))
        active = active_speech_rms_dbfs(x)
        return rms, peak, active

    def _load_and_normalize_voice(label: str, src_path: pathlib.Path) -> np.ndarray:
        raw = load_via_ffmpeg(src_path)
        rms_in, pk_in, active_in = _stats_db(raw)
        normalized, gain_db, binding = normalize_voice(
            raw, VOICE_TARGET_RMS_DBFS, VOICE_PEAK_CEILING_DBFS)
        rms_out, pk_out, active_out = _stats_db(normalized)
        print(f"# {label:<10} in: active_rms={active_in:+6.1f} pk={pk_in:+6.1f} dBFS  "
              f"→ {gain_db:+6.2f} dB ({binding}-bound)  "
              f"out: active_rms={active_out:+6.1f} pk={pk_out:+6.1f} dBFS")
        # Warn when the recording itself is undermastered — peak well below
        # typical-speech levels means even with peak-bound gain we can't lift
        # the active RMS to target. The user should re-record with higher mic
        # input gain.
        TYPICAL_PEAK_DBFS = -6.0
        # Only warn for user-supplied recordings; synthetic and ambient
        # sources have intentionally varying levels (cafe babble's peak
        # depends on the loudest moment in the recording, not on whether
        # the user "spoke loudly enough").
        if pk_in < TYPICAL_PEAK_DBFS - 3.0 and src_path.name.endswith("_recorded.wav"):
            print(f"#   warning: {src_path.name} has peak {pk_in:+.1f} dBFS, much "
                  f"below typical speech ({TYPICAL_PEAK_DBFS:+.0f} dBFS). The "
                  f"composer is peak-bound at {VOICE_PEAK_CEILING_DBFS:+.0f} dBFS "
                  f"so active RMS stops at {active_out:+.1f} (target was "
                  f"{VOICE_TARGET_RMS_DBFS:+.0f}). Re-record with system mic gain "
                  f"higher (macOS: System Settings → Sound → Input) — aim for "
                  f"peaks of -3 to -6 dBFS during loud syllables.")
        return normalized

    # ------------------------- Build the ref stream -------------------------
    ref_path, _ = resolved["ref_voice"]
    assert ref_path is not None
    ref_src = _load_and_normalize_voice("ref_voice", ref_path)
    ref_full = loop_or_clip(ref_src, n_total)
    write_wav_mono_16k(out_dir / f"{out_prefix}_ref.wav", ref_full)

    # ------------------------- Build the mic stream -------------------------
    # 1. Near-end voice (looped voice_synth) — what AEC must preserve
    near_path, _ = resolved["near_voice"]
    assert near_path is not None
    near_src = _load_and_normalize_voice("near_voice", near_path)
    near_full = loop_or_clip(near_src, n_total)
    near_gain = 10.0 ** (NEAR_GAIN_DB / 20.0)

    # 2a. Direct-coupling path: ref voice into mic at a small fixed delay,
    # no IR convolution. Dominates the early echo signal and is what makes
    # the caller voice perceptibly recognizable in the before file.
    direct_delay_n = int(DIRECT_COUPLING_DELAY_MS * 1e-3 * SR)
    direct_gain_lin = 10.0 ** (DIRECT_COUPLING_GAIN_DB / 20.0)
    direct_full = np.zeros(n_total, dtype=np.float64)
    if direct_delay_n < n_total:
        copy_n = n_total - direct_delay_n
        direct_full[direct_delay_n:] = direct_gain_lin * ref_full[:copy_n]

    # 2b. Echo path: ref convolved with cabin IR, scaled (the reverberant tail).
    cabin_ir_path, _ = resolved["cabin_ir"]
    assert cabin_ir_path is not None
    cabin_ir = load_via_ffmpeg(cabin_ir_path)
    echo_full = np.convolve(ref_full, cabin_ir)[:n_total] * ECHO_GAIN

    mic = near_gain * near_full + direct_full + echo_full
    # Parallel buffers used by the two live-test paths:
    #
    #   noise_only_raw  — per-scene noise overlays at their native scene gains.
    #                     Summed with ref_full to produce demo_60s_speaker_mix.wav,
    #                     which `ecnr_live --stimulus demo_60s_speaker_mix.wav`
    #                     plays through the speakers. The AEC reference is the
    #                     same combined stream, so AEC adapts to and cancels
    #                     BOTH the caller voice echo AND the scene-noise echo
    #                     coming from the speakers — realistic for "noisy
    #                     head-unit playback during a call."
    #
    #   noise_only_boosted (noise_only_raw + 6 dB, peak-capped) — same content
    #                     but louder, for the `--inject-noise` path where the
    #                     noise is software-mixed into the mic stream rather
    #                     than played through the speakers. The boost makes it
    #                     audible over live voice + speaker echo without the
    #                     user having to crank --inject-gain-db.
    noise_only_raw = np.zeros(n_total, dtype=np.float64)

    # 3. Per-scene noise overlays
    print("# applying scenes:")
    for start_s, dur_s, name, key, gain_db in scenes:
        path, kind = resolved[key]
        if path is None:
            print(f"  SKIP    {start_s:5.1f}–{start_s + dur_s:5.1f}s  {name}  (file missing)")
            continue

        scene_n = int(dur_s * SR)
        start_n = int(start_s * SR)
        if start_n + scene_n > n_total:
            scene_n = n_total - start_n
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
        contribution = gain_lin * clip_faded
        mic[start_n:start_n + scene_n] += contribution
        noise_only_raw[start_n:start_n + scene_n] += contribution

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

    # demo_60s_noise.wav — boosted version of noise_only_raw for the
    # `ecnr_live --inject-noise` path (software-mixed into the captured mic).
    NOISE_ONLY_BOOST_DB = 6.0
    noise_only_boosted = noise_only_raw * (10.0 ** (NOISE_ONLY_BOOST_DB / 20.0))
    n_peak = float(np.max(np.abs(noise_only_boosted)))
    if n_peak > 0.89:  # leave 1 dB headroom
        noise_only_boosted *= 0.89 / n_peak

    # demo_60s_speaker_mix.wav — caller voice + scene noises summed at their
    # native scene gains. `ecnr_live --stimulus demo_60s_speaker_mix.wav` plays
    # this through the speakers. Since `ecnr_live` uses the stimulus as the
    # AEC reference, AEC has full knowledge of everything the speaker is
    # playing and adapts to cancel both caller echo and noise echo as one
    # composite signal. The user's live mic voice is what survives.
    speaker_mix = ref_full + noise_only_raw
    sm_peak = float(np.max(np.abs(speaker_mix)))
    if sm_peak > 0.99:
        speaker_mix *= 0.99 / sm_peak

    write_wav_mono_16k(out_dir / f"{out_prefix}_mic.wav", mic)
    write_wav_mono_16k(out_dir / f"{out_prefix}_noise.wav", noise_only_boosted)
    write_wav_mono_16k(out_dir / f"{out_prefix}_speaker_mix.wav", speaker_mix)
    print()
    print(f"wrote {out_dir}/{out_prefix}_{{ref,mic,noise,speaker_mix}}.wav  "
          f"({duration_s:.0f}s @ {SR // 1000} kHz mono)")
    print()
    print("next: ./build/ecnr_bench \\")
    print(f"        --mic {out_dir}/{out_prefix}_mic.wav \\")
    print(f"        --ref {out_dir}/{out_prefix}_ref.wav \\")
    print(f"        --out /tmp/{out_prefix}_after.wav")
    print(f"      afplay {out_dir}/{out_prefix}_mic.wav   # before")
    print(f"      afplay /tmp/{out_prefix}_after.wav        # after")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
