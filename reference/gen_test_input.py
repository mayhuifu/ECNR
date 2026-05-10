#!/usr/bin/env python3
"""Synthesize a deterministic mic WAV by mixing voice + simulated speaker echo + noise.

This is the "no-human-in-the-loop" companion to `ecnr_live`. Given:

    voice (clean near-end, e.g. a recording or `voice_synth.wav`)
  + echo  (a far-end stimulus convolved with a short synthetic room IR)
  + noise (optional background noise — road, hvac, bark, ...)

it produces a single 16 kHz mono int16 WAV that `ecnr_bench` can process
together with the matching `ref.wav` (the speaker-side stimulus). The result is
a fully reproducible end-to-end AEC + NS test input: change the noise file or
gain and re-run, and any difference in the bench output is attributable to the
chain, not to the room or the human speaker.

Voice fallback: if `--voice` is omitted, prefers `reference/synth/voice_recorded.wav`
if it exists (i.e. the user has recorded their own voice via
`ecnr_live --record-voice`), otherwise falls back to the synthetic
`reference/synth/voice_synth.wav`. So a fresh checkout works immediately and
upgrades to the user's own voice automatically once they record.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys
import wave

import numpy as np  # type: ignore

SR = 16000


def _read_wav_mono_16k(path: pathlib.Path) -> np.ndarray:
    """Load a 16 kHz mono int16 WAV into a float64 array in [-1, 1].

    Raises ValueError with a clear message if the WAV doesn't match.
    """
    with wave.open(str(path), "rb") as w:
        n_channels = w.getnchannels()
        sample_rate = w.getframerate()
        sampwidth = w.getsampwidth()
        n_frames = w.getnframes()
        if n_channels != 1:
            raise ValueError(
                f"{path}: expected mono (1 channel), got {n_channels} channels"
            )
        if sample_rate != SR:
            raise ValueError(
                f"{path}: expected {SR} Hz, got {sample_rate} Hz"
            )
        if sampwidth != 2:
            raise ValueError(
                f"{path}: expected 16-bit PCM (sampwidth=2), got sampwidth={sampwidth}"
            )
        raw = w.readframes(n_frames)
    pcm = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    return pcm


def _write_wav_mono_16k(path: pathlib.Path, samples: np.ndarray) -> None:
    samples = np.clip(samples, -1.0, 1.0)
    pcm = (samples * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def _make_room_ir(ir_len_ms: int, rng: np.random.Generator) -> np.ndarray:
    """Synthetic exponential-decay room IR — same shape as gen_synth.py uses.

    The seed is fixed so the echo path is deterministic across runs.
    """
    ir_len = int(ir_len_ms * 1e-3 * SR)
    if ir_len < 1:
        ir_len = 1
    ir = rng.standard_normal(ir_len) * np.exp(-np.arange(ir_len) / (0.005 * SR))
    n = float(np.linalg.norm(ir) + 1e-9)
    ir /= n
    return ir


def _resolve_voice_default() -> pathlib.Path | None:
    """Pick the default voice WAV: recorded if present, else synth, else None."""
    base = pathlib.Path("reference/synth")
    recorded = base / "voice_recorded.wav"
    if recorded.exists():
        return recorded
    synth = base / "voice_synth.wav"
    if synth.exists():
        return synth
    return None


def main() -> int:
    p = argparse.ArgumentParser(
        description="Mix voice + simulated speaker echo + noise into a single mic WAV.",
    )
    p.add_argument(
        "--voice",
        default=None,
        help=(
            "clean voice WAV (16 kHz mono). Default: reference/synth/voice_recorded.wav "
            "if present, else reference/synth/voice_synth.wav."
        ),
    )
    p.add_argument(
        "--stimulus",
        default="reference/synth/ref.wav",
        help="speaker-side stimulus WAV that gets echoed (default: reference/synth/ref.wav)",
    )
    p.add_argument(
        "--noise",
        default=None,
        help="optional background noise WAV mixed in after echo (16 kHz mono)",
    )
    p.add_argument(
        "--noise-gain-db",
        type=float,
        default=-12.0,
        help="gain (dB) applied to noise before mix (default: -12.0)",
    )
    p.add_argument(
        "--echo-gain",
        type=float,
        default=0.5,
        help="linear gain on the echoed stimulus, simulating speaker→mic acoustic level (default: 0.5)",
    )
    p.add_argument(
        "--echo-ir-len-ms",
        type=int,
        default=30,
        help="synthetic IR length in ms (default: 30, matches gen_synth.py)",
    )
    p.add_argument(
        "--out",
        default="reference/synth/test_mic.wav",
        help="output mic WAV path (default: reference/synth/test_mic.wav)",
    )
    p.add_argument(
        "--duration",
        type=float,
        default=None,
        help="cap output length in seconds (default: min of voice/stimulus durations)",
    )
    args = p.parse_args()

    # Resolve voice path with fallback.
    if args.voice is None:
        voice_path = _resolve_voice_default()
        if voice_path is None:
            print(
                "error: no voice WAV found at reference/synth/voice_recorded.wav "
                "or reference/synth/voice_synth.wav.\n"
                "  Generate a synthetic voice:    python3 reference/gen_synth.py --voice-only\n"
                "  Or record your own:            ./build/ecnr_live --record-voice "
                "reference/synth/voice_recorded.wav --duration 15",
                file=sys.stderr,
            )
            return 2
    else:
        voice_path = pathlib.Path(args.voice)
        if not voice_path.exists():
            print(f"error: voice file not found: {voice_path}", file=sys.stderr)
            return 2

    stimulus_path = pathlib.Path(args.stimulus)
    if not stimulus_path.exists():
        print(f"error: stimulus file not found: {stimulus_path}", file=sys.stderr)
        return 2

    # Load + validate inputs.
    try:
        voice = _read_wav_mono_16k(voice_path)
        stimulus = _read_wav_mono_16k(stimulus_path)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    noise = None
    if args.noise is not None:
        noise_path = pathlib.Path(args.noise)
        if not noise_path.exists():
            print(f"error: noise file not found: {noise_path}", file=sys.stderr)
            return 2
        try:
            noise = _read_wav_mono_16k(noise_path)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2

    # Determine output length: explicit --duration wins; otherwise min of inputs.
    if args.duration is not None:
        output_len = int(args.duration * SR)
    else:
        output_len = min(len(voice), len(stimulus))
    if output_len <= 0:
        print("error: output length is zero (empty inputs?)", file=sys.stderr)
        return 2

    # --- Echo path: convolve stimulus with a short room IR, scale by echo_gain. ---
    rng = np.random.default_rng(0xECA5)
    ir = _make_room_ir(args.echo_ir_len_ms, rng)
    # Use only the first output_len samples of stimulus before convolution; this
    # keeps the convolution cost proportional to the output duration regardless
    # of input length.
    stim_clip = stimulus[:output_len]
    echoed = np.convolve(stim_clip, ir)[:output_len] * args.echo_gain

    # --- Voice: take first output_len samples (zero-pad if too short). ---
    if len(voice) >= output_len:
        voice_clip = voice[:output_len]
    else:
        voice_clip = np.zeros(output_len, dtype=np.float64)
        voice_clip[:len(voice)] = voice

    # --- Noise (optional): tile to output_len, apply gain. ---
    if noise is not None and len(noise) > 0:
        if len(noise) >= output_len:
            noise_clip = noise[:output_len]
        else:
            reps = (output_len + len(noise) - 1) // len(noise)
            noise_clip = np.tile(noise, reps)[:output_len]
        linear_gain = 10.0 ** (args.noise_gain_db / 20.0)
        noise_clip = noise_clip * linear_gain
    else:
        noise_clip = np.zeros(output_len, dtype=np.float64)

    # --- Mix and clip to int16. Warn on saturation. ---
    mic = voice_clip + echoed + noise_clip
    peak = float(np.max(np.abs(mic)))
    if peak >= 1.0:
        print(
            f"warning: mixed signal peak={peak:.3f} reached/exceeded full-scale; "
            f"clipping to int16. Consider lowering --echo-gain or --noise-gain-db.",
            file=sys.stderr,
        )

    _write_wav_mono_16k(pathlib.Path(args.out), mic)

    duration_s = output_len / SR
    if noise is not None:
        noise_summary = f"noise({args.noise}, {args.noise_gain_db:.1f} dB)"
    else:
        noise_summary = "noise(none)"
    print(
        f"wrote {args.out}: voice={voice_path} + echo(stimulus={stimulus_path}, "
        f"gain={args.echo_gain:.2f}, ir={args.echo_ir_len_ms}ms) + {noise_summary}  "
        f"({duration_s:.1f}s @ {SR // 1000} kHz)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
