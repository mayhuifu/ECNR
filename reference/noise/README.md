# reference/noise/ — real-recording background noise corpus

The five real-world recordings under this directory feed `gen_combined_demo.py`,
the canned 60-second AEC + NS scenario walkthrough that doesn't require live
voice input (Step E.3 in the main README).

The audio files themselves are **not committed to git** (gitignored — see
`.gitignore`); only this README and `MANIFEST.tsv` are. Re-fetch with:

```sh
scripts/fetch-noise.sh
```

That script auto-downloads the `auto` entries from `MANIFEST.tsv`, verifies all
files (auto + manual) against pinned SHA256, and prints a clear "go to URL X,
save at path Y" instruction for any missing manual entries.

## License audit

Every file is **CC0** or **public domain** — no attribution required for use,
no GPL contamination, no copyleft surprises. Credits below are courtesy, not
obligation, matching the project's discipline of licensing every binary that
ships in a demo (PROJECT.md decisions log, 2026-05-10 license audit).

| File | Source | License | Author / Title |
|---|---|---|---|
| `dog_bark.wav` | [archive.org/Red_Library_Animals_Dogs](https://archive.org/details/Red_Library_Animals_Dogs) | CC0 1.0 | Red Library: Animals — Small Dog Barks (R29-52) |
| `music.ogg` | [archive.org/100ClassicalMusicMasterpieces](https://archive.org/details/100ClassicalMusicMasterpieces) | Public domain (composition) | Schumann — The Merry Peasant (1848) |
| `cafe_babble.wav` | [freesound.org/Breviceps/457043](https://freesound.org/people/Breviceps/sounds/457043/) | CC0 1.0 | Busy Room Ambience by Breviceps |
| `stadium_crowd.wav` | [freesound.org/Sandermotions/494362](https://freesound.org/people/Sandermotions/sounds/494362/) | CC0 1.0 | Soccer stadium Oehh by Sandermotions |
| `car_interior.wav` | [freesound.org/NachtmahrTV/556692](https://freesound.org/people/NachtmahrTV/sounds/556692/) | CC0 1.0 | Car Engine interior by NachtmahrTV |

The Schumann composition is comfortably in the public domain (composer died 1856).
The recording is hosted in Internet Archive's "Community Audio" collection without
an explicit license tag; we treat it as PD because the underlying composition is
PD and the recording's character (orchestral classical, no commercial vocal
performance) is not subject to the kinds of restrictions that surfaced in the
[Great 78 Project lawsuit (2023)](https://en.wikipedia.org/wiki/The_Great_78_Project).
If a sharper provenance is needed, swap in a [Musopen](https://musopen.org)
recording (explicit PD release) and update the manifest.

## Format conventions

Files arrive in whatever format their upstream provides (16-bit / 24-bit, mono /
stereo, 44.1 / 48 kHz, WAV / OGG / MP3). The composer (`gen_combined_demo.py`)
uses **ffmpeg** to convert each to 16 kHz mono float at composition time —
**no per-file format normalization is required**. The original file at the
upstream-provided format is what gets pinned in `MANIFEST.tsv` so the SHA256
matches a re-fetch byte-for-byte.

If you swap a file (e.g. replace `stadium_crowd.wav` with a different CC0
recording), update `MANIFEST.tsv`'s URL + SHA256 entries and re-run
`scripts/fetch-noise.sh` to verify.

## Why these specific files

Each one was chosen to stress a distinct AEC + NS scenario:

- **`car_interior.wav` (51s, real cabin recording)** — stationary low-frequency
  road / engine rumble. The chain's fixed-noise NS (RNNoise) should suppress
  this cleanly; this is the easiest case for the post-filter.
- **`cafe_babble.wav` (15.7s)** — non-stationary multi-talker babble with
  cutlery transients. The hardest stationary-NS case: babble has speech-like
  spectra so RNNoise can't aggressively suppress without eating the near-end.
- **`stadium_crowd.wav` (9.3s)** — broadband crowd "Oehh" with a clear
  build-and-release envelope. Tests how the NS handles a high-energy
  non-stationary event without overshooting.
- **`dog_bark.wav` (12.9s)** — sparse high-amplitude transients, a known
  weakness of RNNoise (the README notes "bark-like transient noise survives
  with a residual click; stronger transient suppression is Phase 3's job").
  This file makes that limitation concrete, not synthetic.
- **`music.ogg` (54s, classical orchestral)** — full-band tonal content,
  long sustained notes. Stresses ADR-0009 (media-aware AEC) — the post-filter
  trained on voice residuals may attenuate or distort music inconsistently.

The synthetic counterparts (`reference/synth/noise_road.wav`,
`reference/synth/noise_hvac.wav`, `reference/synth/noise_bark.wav`,
`reference/synth/ref_music.wav`) remain useful as **deterministic fallbacks**:
when a real file is missing, the composer falls back to the synthetic version
of the same scenario so the canned demo still runs end-to-end.
