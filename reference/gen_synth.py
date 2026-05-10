#!/usr/bin/env python3
"""Generate a synthetic 16 kHz mic/ref pair for the Phase 0 bench harness.

ref.wav        = white noise (the "far-end" stimulus, what would play out the speaker)
mic.wav        = ref convolved with a short synthetic IR + low-amplitude near-end speech-like tone
                 (stand-in for "speech under echo"; not real speech)
near_clean.wav = the near-end tone alone (oracle target)

Plus three synthetic background-noise WAVs used by `ecnr_live --inject-noise` to
demo the AEC + NS chain's noise-suppression behavior:

noise_road.wav = pink-ish broadband rumble with a low-frequency wobble
noise_bark.wav = 4-8 short band-limited bursts (dog-bark-shaped)
noise_hvac.wav = 60 Hz hum + harmonics over a low-level pink noise floor

A synthetic-voice WAV used by `gen_test_input.py` as the deterministic
near-end source when a real recording isn't available:

voice_synth.wav = continuous voiced/unvoiced synthetic speech (F0 ~110 Hz with
                  drift, formant peaks at ~700/1100/2400/3500 Hz, 5 Hz syllable
                  envelope, ~20% silences, normalized to -18 dBFS RMS)

And two alternative far-end stimuli for the live demo (Step E in the README) —
their content is recognizable, so when AEC3 cancels them you can hear the
result instead of just "less white noise":

ref_voice.wav  = TTS-prompt-style synthetic voice (F0 ~200 Hz with drift,
                 different formant set from voice_synth, faster syllables,
                 shorter silences). Read as "the caller on the other end."
ref_music.wav  = simple I-V-vi-IV chord progression in C major (~2 s/chord)
                 with sustained triads + an arpeggio melody on top. Stresses
                 AEC's full-band behaviour and is the reference case for
                 ADR-0009 (media-aware AEC).

All outputs are 16 kHz mono int16, length matched to --duration. The pair is
time-aligned at sample 0; AEC's delay estimator handles the rest.
"""

from __future__ import annotations

import argparse
import math
import pathlib
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


def _normalize_rms(x: np.ndarray, target_dbfs: float) -> np.ndarray:
    """Scale `x` so its RMS equals `target_dbfs` (relative to full-scale 1.0)."""
    rms = float(np.sqrt(np.mean(x * x)) + 1e-12)
    target = 10.0 ** (target_dbfs / 20.0)
    return x * (target / rms)


def _pink_noise(n: int, rng: np.random.Generator) -> np.ndarray:
    """Cheap pink-ish noise via single-pole IIR low-pass on white noise.

    Not spectrally exact 1/f, but emphasizes low frequencies enough to read as
    "rumble" / room-noise floor for demo purposes.
    """
    white = rng.standard_normal(n)
    out = np.empty(n, dtype=np.float64)
    alpha = 0.95
    state = 0.0
    for i in range(n):
        state = alpha * state + (1.0 - alpha) * white[i]
        out[i] = state
    # Re-normalize to unit-ish amplitude before the caller scales it.
    peak = float(np.max(np.abs(out))) + 1e-12
    return out / peak


def gen_road(n: int, rng: np.random.Generator) -> np.ndarray:
    """Synthetic road rumble: pink noise + slow low-frequency wobble."""
    pink = _pink_noise(n, rng)
    t = np.arange(n) / SR
    # 60-90 Hz tone with slow amplitude modulation (suspension/road wobble).
    base_hz = 75.0
    am = 0.5 + 0.5 * np.sin(2.0 * math.pi * 0.7 * t)
    rumble = 0.5 * np.sin(2.0 * math.pi * base_hz * t) * am
    mix = 0.7 * pink + 0.3 * rumble
    return _normalize_rms(mix, target_dbfs=-20.0)


def gen_bark(n: int, rng: np.random.Generator) -> np.ndarray:
    """Synthetic dog-bark bursts placed at random positions."""
    out = np.zeros(n, dtype=np.float64)
    duration_s = n / SR

    # Number of barks, with at least ~0.5 s of leading silence.
    n_barks = int(rng.integers(4, 9))
    cursor_s = float(rng.uniform(0.3, 1.0))

    attack_s, hold_s, decay_s = 0.020, 0.050, 0.200
    bark_total_s = attack_s + hold_s + decay_s

    for _ in range(n_barks):
        if cursor_s + bark_total_s >= duration_s:
            break
        bark_len = int(bark_total_s * SR)
        # Band-limited noise: white * narrow-band carrier (centered ~1.2 kHz).
        white = rng.standard_normal(bark_len)
        tt = np.arange(bark_len) / SR
        center_hz = float(rng.uniform(700.0, 1800.0))
        carrier = np.cos(2.0 * math.pi * center_hz * tt)
        # Multiplying white noise with a tone gives a band-limited noise burst
        # centered around `center_hz` after envelope shaping. Crude but reads as
        # "bark-y" without any FFT/FIR machinery.
        burst = white * carrier
        # Soft normalize within the burst.
        peak = float(np.max(np.abs(burst))) + 1e-12
        burst /= peak

        # ADSR envelope: linear attack, flat hold, exponential decay.
        env = np.empty(bark_len, dtype=np.float64)
        a_n = int(attack_s * SR)
        h_n = int(hold_s * SR)
        d_n = bark_len - a_n - h_n
        env[:a_n] = np.linspace(0.0, 1.0, a_n, endpoint=False)
        env[a_n:a_n + h_n] = 1.0
        # Exponential decay to ~-40 dB by end of decay region.
        env[a_n + h_n:] = np.exp(np.linspace(0.0, -4.6, d_n))
        shaped = burst * env

        start = int(cursor_s * SR)
        # Aim for peak ~-6 dBFS (linear 0.5).
        out[start:start + bark_len] += 0.5 * shaped

        gap_s = float(rng.uniform(1.0, 3.0))
        cursor_s += bark_total_s + gap_s

    # Clamp to [-1, 1] just in case overlap pushes peaks past 1.0.
    return np.clip(out, -1.0, 1.0)


def gen_hvac(n: int, rng: np.random.Generator) -> np.ndarray:
    """60 Hz hum + harmonics over a low-level pink noise floor."""
    t = np.arange(n) / SR
    # Harmonic amplitudes from -3 / -9 / -15 / -21 dB relative to a reference.
    harmonics = [(60.0, -3.0), (120.0, -9.0), (180.0, -15.0), (240.0, -21.0)]
    hum = np.zeros(n, dtype=np.float64)
    for f, db in harmonics:
        amp = 10.0 ** (db / 20.0)
        hum += amp * np.sin(2.0 * math.pi * f * t)
    # Normalize the hum stack to ~unit peak before scaling.
    hum_peak = float(np.max(np.abs(hum))) + 1e-12
    hum /= hum_peak

    floor = _pink_noise(n, rng)
    floor = _normalize_rms(floor, target_dbfs=-25.0)

    mix = 0.7 * hum + floor
    return _normalize_rms(mix, target_dbfs=-22.0)


def gen_voice_synth(n: int, rng: np.random.Generator) -> np.ndarray:
    """Continuous voiced/unvoiced synthetic speech.

    Statistics roughly mimic real speech: F0 ~110 Hz with slow drift, formants at
    ~700/1100/2400/3500 Hz, 5 Hz syllable amplitude modulation, voiced/unvoiced
    switching at ~3 Hz with random phasing, and ~20% silences ("between words")
    with a 100 ms minimum gap. Output is normalized to -18 dBFS RMS.

    No FIR/IIR filter banks: voiced segments are a sum of harmonics-of-F0 with
    amplitudes shaped by a simple resonance kernel evaluated at each harmonic
    frequency (cheap "formant filter" via per-harmonic gain). Unvoiced segments
    are band-limited noise centered ~3-5 kHz (sibilant-like).
    """
    t = np.arange(n) / SR

    # --- F0 contour: 110 Hz with ±15 Hz slow drift over the segment. ---
    # Sum of two slow sinusoids gives an aperiodic-looking drift without LFOs
    # that lock to the segment boundary.
    drift = (np.sin(2.0 * math.pi * 0.13 * t + rng.uniform(0.0, 2.0 * math.pi))
             + np.sin(2.0 * math.pi * 0.21 * t + rng.uniform(0.0, 2.0 * math.pi)))
    drift /= float(np.max(np.abs(drift)) + 1e-12)
    f0 = 110.0 + 15.0 * drift  # Hz, per-sample

    # Cumulative phase for F0 — required so we can sum harmonics whose
    # instantaneous frequency varies sample-by-sample.
    phase = 2.0 * math.pi * np.cumsum(f0) / SR

    # --- Formant model: peaks at ~700, 1100, 2400, 3500 Hz. ---
    # Each formant contributes a Lorentzian-shaped gain centered at its frequency
    # with a per-formant bandwidth. A harmonic at frequency `fh` gets weight
    # sum_i A_i / (1 + ((fh - f_i) / bw_i)^2).
    formants = [(700.0, 100.0, 1.0),
                (1100.0, 100.0, 0.8),
                (2400.0, 150.0, 0.5),
                (3500.0, 200.0, 0.3)]

    def formant_gain(fh: np.ndarray) -> np.ndarray:
        g = np.zeros_like(fh)
        for f_c, bw, amp in formants:
            g += amp / (1.0 + ((fh - f_c) / bw) ** 2)
        return g

    # --- Voiced signal: sum harmonics of F0 (up to Nyquist), each weighted by
    # the formant envelope evaluated at its instantaneous frequency. To keep
    # this O(n_harmonics * n) without numerical games, evaluate the formant
    # gain at the *mean* F0 of each harmonic and treat it as a constant. The
    # drift is small enough that this is visually indistinguishable from the
    # exact time-varying gain.
    n_harmonics = 40
    voiced = np.zeros(n, dtype=np.float64)
    f0_mean = float(np.mean(f0))
    for k in range(1, n_harmonics + 1):
        fh = k * f0_mean
        if fh >= 0.45 * SR:  # below Nyquist with a small guard band
            break
        g = float(formant_gain(np.array([fh]))[0])
        # 1/k roll-off on top of formant gain is a crude source-spectrum tilt
        # (real glottal sources are ~-12 dB/oct; close enough for this generator).
        voiced += (g / k) * np.sin(k * phase + rng.uniform(0.0, 2.0 * math.pi))
    # Normalize voiced amplitude so it doesn't dominate the unvoiced segments.
    voiced_peak = float(np.max(np.abs(voiced)) + 1e-12)
    voiced /= voiced_peak

    # --- Unvoiced signal: band-limited noise centered ~3-5 kHz. ---
    # Multiplying white noise by a 4 kHz carrier shifts its spectrum to that
    # region (same trick as gen_bark). A second multiplicative envelope at
    # ~1 kHz widens it slightly so it doesn't read as a pure tone.
    white = rng.standard_normal(n)
    carrier_hi = np.cos(2.0 * math.pi * 4000.0 * t)
    unvoiced = white * carrier_hi
    unv_peak = float(np.max(np.abs(unvoiced)) + 1e-12)
    unvoiced /= unv_peak

    # --- Voiced/unvoiced switching at ~3 Hz with random phasing. ---
    # 3 Hz square-ish envelope (sign of a sine) gives ~167 ms voiced runs
    # alternating with ~167 ms unvoiced runs. Random phase de-aligns segment
    # boundaries from the syllable envelope so the result doesn't sound metronomic.
    vu_phase = rng.uniform(0.0, 2.0 * math.pi)
    vu_sw = np.sin(2.0 * math.pi * 3.0 * t + vu_phase)
    voiced_mask = (vu_sw > 0.0).astype(np.float64)
    # Smooth the mask edges over ~10 ms to avoid clicks at switch boundaries.
    smooth_n = max(1, int(0.010 * SR))
    kernel = np.ones(smooth_n) / smooth_n
    voiced_mask = np.convolve(voiced_mask, kernel, mode="same")
    unvoiced_mask = 1.0 - voiced_mask

    speech = voiced * voiced_mask + 0.6 * unvoiced * unvoiced_mask

    # --- Syllable-rate envelope at ~5 Hz. ---
    syl_phase = rng.uniform(0.0, 2.0 * math.pi)
    syl_env = 0.4 + 0.6 * (0.5 + 0.5 * np.sin(2.0 * math.pi * 5.0 * t + syl_phase))
    speech *= syl_env

    # --- Silence regions: ~20% of the duration, randomly placed, min-gap 100 ms.
    # Walk the timeline placing silence "windows" of random length in [80, 350] ms
    # until the cumulative silent duration hits 20% of total. Each silence window
    # ramps to zero with 5 ms cosine fades to avoid clicks.
    silence_mask = np.ones(n, dtype=np.float64)
    target_silent = int(0.20 * n)
    placed_silent = 0
    min_gap_n = int(0.100 * SR)
    fade_n = int(0.005 * SR)
    cursor = int(rng.uniform(0.0, 0.5 * SR))  # small leading non-silence
    safety = 0
    while placed_silent < target_silent and safety < 100:
        safety += 1
        win_n = int(rng.uniform(0.080, 0.350) * SR)
        if cursor + win_n >= n:
            break
        # Cosine-faded silence window: full silence in the middle, smooth ramps
        # at the edges.
        env = np.ones(win_n)
        if win_n > 2 * fade_n:
            ramp_in = 0.5 * (1.0 + np.cos(np.linspace(0.0, math.pi, fade_n)))
            ramp_out = 0.5 * (1.0 + np.cos(np.linspace(math.pi, 0.0, fade_n)))
            env[:fade_n] = ramp_in
            env[win_n - fade_n:] = ramp_out
        silence_mask[cursor:cursor + win_n] *= env
        # Truly zero out the central plateau (between the two ramps).
        if win_n > 2 * fade_n:
            silence_mask[cursor + fade_n:cursor + win_n - fade_n] = 0.0
        placed_silent += win_n
        gap = min_gap_n + int(rng.uniform(0.0, 0.500 * SR))
        cursor += win_n + gap

    speech *= silence_mask

    # Normalize overall RMS to -18 dBFS. Skip pure-silence pathological case.
    rms = float(np.sqrt(np.mean(speech * speech)) + 1e-12)
    if rms > 1e-9:
        speech = _normalize_rms(speech, target_dbfs=-18.0)
    return np.clip(speech, -1.0, 1.0)


def gen_voice_render(n: int, rng: np.random.Generator) -> np.ndarray:
    """TTS-prompt-style synthetic voice intended as a far-end render stimulus.

    Distinguished from `gen_voice_synth` (the near-end speaker) by F0, formant
    set, syllable cadence, and silence fraction so the two are easily separable
    by ear when both are present. Roughly: a higher voice ("the caller"),
    reading a continuous prompt at a brisk pace with few pauses.

    F0 ~200 Hz with drift, formants at ~900/1400/2900/4000 Hz, 6 Hz syllable
    rate, ~10% silences. Output normalized to -16 dBFS RMS — slightly hotter
    than voice_synth so the speaker drives a meaningful echo level.
    """
    t = np.arange(n) / SR

    drift = (np.sin(2.0 * math.pi * 0.17 * t + rng.uniform(0.0, 2.0 * math.pi))
             + np.sin(2.0 * math.pi * 0.29 * t + rng.uniform(0.0, 2.0 * math.pi)))
    drift /= float(np.max(np.abs(drift)) + 1e-12)
    f0 = 200.0 + 25.0 * drift

    phase = 2.0 * math.pi * np.cumsum(f0) / SR

    formants = [(900.0, 110.0, 1.0),
                (1400.0, 110.0, 0.85),
                (2900.0, 170.0, 0.55),
                (4000.0, 220.0, 0.35)]

    def formant_gain(fh: np.ndarray) -> np.ndarray:
        g = np.zeros_like(fh)
        for f_c, bw, amp in formants:
            g += amp / (1.0 + ((fh - f_c) / bw) ** 2)
        return g

    n_harmonics = 30
    voiced = np.zeros(n, dtype=np.float64)
    f0_mean = float(np.mean(f0))
    for k in range(1, n_harmonics + 1):
        fh = k * f0_mean
        if fh >= 0.45 * SR:
            break
        g = float(formant_gain(np.array([fh]))[0])
        voiced += (g / k) * np.sin(k * phase + rng.uniform(0.0, 2.0 * math.pi))
    voiced /= float(np.max(np.abs(voiced)) + 1e-12)

    white = rng.standard_normal(n)
    carrier_hi = np.cos(2.0 * math.pi * 4500.0 * t)
    unvoiced = white * carrier_hi
    unvoiced /= float(np.max(np.abs(unvoiced)) + 1e-12)

    vu_phase = rng.uniform(0.0, 2.0 * math.pi)
    vu_sw = np.sin(2.0 * math.pi * 4.0 * t + vu_phase)
    voiced_mask = (vu_sw > 0.0).astype(np.float64)
    smooth_n = max(1, int(0.010 * SR))
    kernel = np.ones(smooth_n) / smooth_n
    voiced_mask = np.convolve(voiced_mask, kernel, mode="same")
    unvoiced_mask = 1.0 - voiced_mask

    speech = voiced * voiced_mask + 0.5 * unvoiced * unvoiced_mask

    syl_phase = rng.uniform(0.0, 2.0 * math.pi)
    syl_env = 0.5 + 0.5 * (0.5 + 0.5 * np.sin(2.0 * math.pi * 6.0 * t + syl_phase))
    speech *= syl_env

    silence_mask = np.ones(n, dtype=np.float64)
    target_silent = int(0.10 * n)
    placed_silent = 0
    fade_n = int(0.005 * SR)
    cursor = int(rng.uniform(0.0, 0.3 * SR))
    safety = 0
    while placed_silent < target_silent and safety < 100:
        safety += 1
        win_n = int(rng.uniform(0.060, 0.180) * SR)
        if cursor + win_n >= n:
            break
        env = np.ones(win_n)
        if win_n > 2 * fade_n:
            ramp_in = 0.5 * (1.0 + np.cos(np.linspace(0.0, math.pi, fade_n)))
            ramp_out = 0.5 * (1.0 + np.cos(np.linspace(math.pi, 0.0, fade_n)))
            env[:fade_n] = ramp_in
            env[win_n - fade_n:] = ramp_out
        silence_mask[cursor:cursor + win_n] *= env
        if win_n > 2 * fade_n:
            silence_mask[cursor + fade_n:cursor + win_n - fade_n] = 0.0
        placed_silent += win_n
        gap = int(0.080 * SR) + int(rng.uniform(0.0, 0.350 * SR))
        cursor += win_n + gap

    speech *= silence_mask

    rms = float(np.sqrt(np.mean(speech * speech)) + 1e-12)
    if rms > 1e-9:
        speech = _normalize_rms(speech, target_dbfs=-16.0)
    return np.clip(speech, -1.0, 1.0)


def gen_music_render(n: int, rng: np.random.Generator) -> np.ndarray:
    """Simple I-V-vi-IV chord progression in C major with sustained triads
    and an arpeggio melody on top. ~2 s/chord, looped across the duration.

    Each note is a fundamental + two harmonics with amplitudes 1.0/0.5/0.25,
    shaped by a per-chord ADSR. The arpeggio adds an 8th-note melody that
    plays the chord tones in a (root, third, fifth, third) pattern. RMS
    normalized to -16 dBFS.
    """
    # I-V-vi-IV in C major. Note frequencies in Hz (A4 = 440).
    progression = [
        # C major: C4 E4 G4
        ((261.63, 329.63, 392.00),),
        # G major: G3 B3 D4
        ((196.00, 246.94, 293.66),),
        # A minor: A3 C4 E4
        ((220.00, 261.63, 329.63),),
        # F major: F3 A3 C4
        ((174.61, 220.00, 261.63),),
    ]
    chord_dur_s = 2.0
    chord_n = int(chord_dur_s * SR)

    def adsr(length: int, attack_s: float, release_s: float) -> np.ndarray:
        env = np.ones(length, dtype=np.float64)
        a = max(1, int(attack_s * SR))
        r = max(1, int(release_s * SR))
        if a + r >= length:
            a = length // 4
            r = length // 4
        env[:a] = np.linspace(0.0, 1.0, a, endpoint=False)
        env[length - r:] = np.linspace(1.0, 0.0, r, endpoint=True)
        return env

    def note(freq_hz: float, length: int, attack_s: float, release_s: float,
             phase0: float) -> np.ndarray:
        tt = np.arange(length) / SR
        # Fundamental + 2nd + 3rd harmonics, amplitudes 1.0 / 0.5 / 0.25.
        wave_f = (np.sin(2.0 * math.pi * freq_hz * tt + phase0)
                  + 0.5 * np.sin(2.0 * math.pi * 2.0 * freq_hz * tt + phase0)
                  + 0.25 * np.sin(2.0 * math.pi * 3.0 * freq_hz * tt + phase0))
        return wave_f * adsr(length, attack_s, release_s)

    out = np.zeros(n, dtype=np.float64)

    # Loop the progression until the buffer is full.
    chord_idx = 0
    cursor = 0
    while cursor < n:
        triad = progression[chord_idx % len(progression)][0]
        end = min(cursor + chord_n, n)
        seg_len = end - cursor

        # Sustained chord: each note across the whole segment.
        chord_seg = np.zeros(seg_len, dtype=np.float64)
        for f in triad:
            phase0 = rng.uniform(0.0, 2.0 * math.pi)
            chord_seg += note(f, seg_len, attack_s=0.025, release_s=0.080, phase0=phase0)
        # Normalize before adding so each chord has roughly equal level.
        chord_peak = float(np.max(np.abs(chord_seg)) + 1e-12)
        chord_seg /= chord_peak

        # Arpeggio melody: 8 eighth-notes per chord (one per ~250 ms at 2 s/chord).
        # Pattern walks the triad as (root, third, fifth, third) twice.
        beats_per_chord = 8
        beat_n = seg_len // beats_per_chord if seg_len >= beats_per_chord else seg_len
        arp_seg = np.zeros(seg_len, dtype=np.float64)
        if beat_n > 0:
            pattern = [0, 1, 2, 1, 0, 1, 2, 1]
            for b in range(beats_per_chord):
                start = b * beat_n
                if start >= seg_len:
                    break
                length = min(beat_n, seg_len - start)
                f = triad[pattern[b]] * 2.0  # one octave up
                phase0 = rng.uniform(0.0, 2.0 * math.pi)
                arp_seg[start:start + length] += note(
                    f, length, attack_s=0.010, release_s=0.060, phase0=phase0)
            arp_peak = float(np.max(np.abs(arp_seg)) + 1e-12)
            arp_seg /= arp_peak

        # Mix sustained chord (louder) + arpeggio (quieter).
        out[cursor:end] = 0.7 * chord_seg + 0.4 * arp_seg

        cursor = end
        chord_idx += 1

    out = _normalize_rms(out, target_dbfs=-16.0)
    return np.clip(out, -1.0, 1.0)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--duration", type=float, default=10.0)
    p.add_argument("--out-dir", default="reference/synth")
    p.add_argument(
        "--noise-only",
        action="store_true",
        help="only generate the three noise WAVs (skip ref/mic/near_clean/voice_synth)",
    )
    p.add_argument(
        "--voice-only",
        action="store_true",
        help="only generate voice_synth.wav (skip everything else)",
    )
    p.add_argument(
        "--render-only",
        action="store_true",
        help="only generate ref_voice.wav and ref_music.wav (skip everything else)",
    )
    args = p.parse_args()

    only_flags = sum(int(x) for x in (args.noise_only, args.voice_only, args.render_only))
    if only_flags > 1:
        print("error: --noise-only / --voice-only / --render-only are mutually exclusive")
        return 2

    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    n = int(args.duration * SR)
    rng = np.random.default_rng(0xECA0)

    # Each ancillary stem uses its own seed so --foo-only output is
    # byte-identical to the corresponding stem from a full run.
    if args.voice_only:
        write_wav(
            out / "voice_synth.wav",
            gen_voice_synth(n, np.random.default_rng(0xECA4)),
        )
        print(
            f"wrote {out}/voice_synth.wav  ({args.duration:.1f}s @ {SR} Hz)"
        )
        return 0

    if args.render_only:
        write_wav(out / "ref_voice.wav", gen_voice_render(n, np.random.default_rng(0xECA5)))
        write_wav(out / "ref_music.wav", gen_music_render(n, np.random.default_rng(0xECA6)))
        print(
            f"wrote {out}/{{ref_voice,ref_music}}.wav  "
            f"({args.duration:.1f}s @ {SR} Hz)"
        )
        return 0

    if not args.noise_only:
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
        write_wav(
            out / "voice_synth.wav",
            gen_voice_synth(n, np.random.default_rng(0xECA4)),
        )
        write_wav(out / "ref_voice.wav", gen_voice_render(n, np.random.default_rng(0xECA5)))
        write_wav(out / "ref_music.wav", gen_music_render(n, np.random.default_rng(0xECA6)))

    # Noise stems use independent RNGs so --noise-only output is byte-identical
    # to the noise files in a full run.
    write_wav(out / "noise_road.wav", gen_road(n, np.random.default_rng(0xECA1)))
    write_wav(out / "noise_bark.wav", gen_bark(n, np.random.default_rng(0xECA2)))
    write_wav(out / "noise_hvac.wav", gen_hvac(n, np.random.default_rng(0xECA3)))

    if args.noise_only:
        print(
            f"wrote {out}/{{noise_road,noise_bark,noise_hvac}}.wav  "
            f"({args.duration:.1f}s @ {SR} Hz)"
        )
    else:
        print(
            f"wrote {out}/{{ref,mic,near_clean,voice_synth,ref_voice,ref_music,"
            f"noise_road,noise_bark,noise_hvac}}.wav  "
            f"({args.duration:.1f}s @ {SR} Hz)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
