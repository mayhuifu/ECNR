# Phase 2 — Cabin Recording Protocol (operational playbook)

> **Companion to** [`docs/phase-2-cabin-characterization-plan.md`](phase-2-cabin-characterization-plan.md). The plan answers "what data do we need"; this doc answers "how do we capture it in the vehicle, action by action." Print + bring on the recording day.
>
> **Status:** Draft (2026-05-19). Sharpens with each recording session — every discovered ambiguity should land back here.

## 0. Why the 4-track-per-condition layout

The `ecnr_eval` harness ([ADR-0011 §3](adr/0011-aec3-tuning-methodology.md)) expects each condition directory to contain **four** WAVs at the same sample rate and length:

```
conditions/<cond_id>/
  mic.wav             # the production mix the chain will see: echo + near-end + noise
  ref.wav             # the far-end render (the AEC reference)
  echo_only_mic.wav   # echo as it arrives at the mic, NO near-end, NO noise above ambient floor
  near_end_clean.wav  # the near-end voice signal alone (with ambient noise mixed in, no echo)
```

To capture all four, **each condition requires four separate takes** in the vehicle, run back-to-back without changing any state between takes (engine on, HVAC unchanged, speed held, mic positions fixed). The four takes are post-processed into the 4-track layout. Skipping any one of them costs us a condition.

## 1. Equipment checklist (pre-trip)

### Required

- **The U300 device** (or a representative mic-array stand-in if U300 not yet available).
- **Stimulus laptop** running `ecnr_live` with the prepared stimulus files. macOS or Linux fine; battery + AC adapter; mounted phone-cradle if possible.
- **Audio interface for direct-from-laptop playback** (USB-C audio interface or aux cable from laptop headphone-out to the head-unit's aux-in). Bluetooth playback is **last resort** — adds variable delay and codec colour that confuses ERLE.
- **Phantom-powered measurement mic** (Earthworks M30 / Behringer ECM8000 / equivalent) **plus a separate audio interface** (Focusrite Scarlett 2i2 or similar) for the calibrated reference capture path. Optional but strongly recommended; without it, every measurement depends on the U300 HAL being correct.
- **Sound-level meter** or calibrated phone app (e.g., NIOSH SLM on iOS) for SPL reference.
- **Tripod or rigid mount** for the reference mic at driver-head position. Hot glue / blue-tac as a backup.
- **Two pairs of high-isolation headphones** for the playback-side operator (the person speaking the near-end voice cannot hear the cabin during near-end-only takes; they monitor via headphones).
- **A printed copy** of this protocol + the condition matrix below.

### Nice to have

- **A second laptop** for live-monitoring the recordings as they happen — catches a bad take before you move on.
- **HATS (Head and Torso Simulator)** if the budget allows; replaces the human near-end speaker for repeatable, calibrated mouth signals. Rentable.
- **Vehicle telemetry** (OBD-II logger or just a phone with GPS speed) so each condition's speed is timestamped, not guessed.

## 2. Pre-recording calibration (parked, engine off)

Do these **once per session**, before driving anywhere.

### 2.1 Sample-rate + level alignment

1. Open `ecnr_live --record-voice /tmp/calib_silence.wav --duration 5` and let it record 5 s of cabin silence. Confirm the WAV is 16 kHz mono int16. If it's not, fix the device config; **don't proceed**.
2. Play a known stimulus through the head-unit at the calibrated volume notch (see §2.2). Record the head-unit output via the reference mic for 10 s. Save as `/tmp/calib_ref_capture.wav`.
3. Compute the RMS of `/tmp/calib_ref_capture.wav`. Adjust head-unit volume so the in-cabin SPL at the driver-head position is **75 dBA** (typical conversational-loudness reference; SLM reading). Note the notch value.

### 2.2 Stimulus file prep

You need one stimulus file that is the **same across all conditions**. Don't change it mid-session; the only thing that varies between conditions is the cabin state, not the signal.

Recommended stimulus: a 60–90 s clip of **diverse content** — speech-shaped noise (5 s, for IR estimation) + a male voice sentence (15 s) + a female voice sentence (15 s) + a male/female overlap (10 s) + a music clip (15 s) + silence (5 s). Put this file at `reference/synth/phase2_stimulus.wav` and pin its sha256 in `reference/MANIFEST.tsv`.

### 2.3 Render-tap delay measurement

Critical for AEC alignment. With engine off, HVAC off, windows closed:

1. Play a 1-second 1 kHz click + 9 seconds of silence through the head-unit.
2. Simultaneously capture via the U300 mic AND the reference mic.
3. Cross-correlate the captured mic stream against the played file. The peak position gives the round-trip delay (laptop-out → head-unit → speaker → cabin → mic → device-in).
4. Note this delay in the session log. It varies by head-unit but is stable across conditions for the same vehicle + session.

If the delay is > 250 ms, that's already a 3GPP TS 26.131 violation; flag and continue but document.

### 2.4 Anechoic baseline (one-time, before the vehicle work)

Per the characterization plan: record the same stimulus + near-end speech in a quiet room (a treated home office, or worst-case a small closet with blankets). This is the "what the cabin adds" reference; without it, you can't separate vehicle-acoustic effects from any other artifact.

## 3. The condition matrix (Phase-2 first-cut: 6 conditions)

| # | ID | Engine | Speed | HVAC | Windows | Music in render | Notes |
|---|---|---|---|---|---|---|---|
| 1 | `c01_idle_quiet` | on | 0 mph | off | closed | no | the floor; lowest noise condition with engine running |
| 2 | `c02_idle_hvac` | on | 0 mph | mid | closed | no | stationary HVAC noise; targets steady-state NS |
| 3 | `c03_city_30mph` | on | ~30 mph (urban) | low | closed | no | road + engine; cruising-speed baseline |
| 4 | `c04_highway_65mph` | on | ~65 mph | low | closed | no | dominant wind + road noise |
| 5 | `c05_highway_windows` | on | ~65 mph | low | half open | no | wind transients; hardest case for NS |
| 6 | `c06_idle_music` | on | 0 mph | off | closed | **yes** | media-aware-AEC torture test (ADR-0009) |

For each condition × 4 takes = **24 recordings minimum**.

## 4. Per-condition recording procedure (the 4 takes)

**Do not change vehicle state mid-condition.** Pull over to set up if needed; resume at the same state for all four takes of the condition.

For each condition `<cond_id>`:

### Take A — SILENCE PASS (ambient noise floor)

Purpose: captures the noise floor of the condition. Used as a reference for ambient-only metrics and to verify the echo-only pass doesn't have hidden near-end speech.

1. Confirm vehicle state per the matrix (speed held within ±3 mph, HVAC at notch, windows positioned, engine status).
2. **Near-end speaker stays silent.** Stimulus laptop plays nothing.
3. Start recording:
   ```sh
   ecnr_live --record-voice /tmp/<cond_id>/A_silence.wav --duration 15
   ```
4. Sit still for 15 s. **No coughs, no rustling, no turn-signal clicks.** If any happen, redo this take.
5. (Optional) Simultaneously capture from the reference mic for the same 15 s.

### Take B — ECHO-ONLY PASS (`echo_only_mic.wav`)

Purpose: captures the echo path with **no near-end speech**. The true-ERLE oracle per [ADR-0011 §1](adr/0011-aec3-tuning-methodology.md).

1. Vehicle state unchanged from Take A.
2. **Near-end speaker stays silent for the entire take.** No throat-clearing, no breathing into the mic.
3. Stimulus laptop plays the phase-2 stimulus file through the head-unit at the calibrated notch.
4. The mic captures the cabin echo of the stimulus + the ambient noise floor.
5. Recording:
   ```sh
   ecnr_live --record-voice /tmp/<cond_id>/B_echo_only.wav --duration <stimulus_length_plus_5s>
   ```
6. Verify post-take: open the WAV in Audacity, scan for any speech-like peaks. Anything that looks like near-end speech → redo.

Common failure: a window-opening burst from condition 5 corrupts an otherwise clean echo-only take. Redo if it happens.

### Take C — NEAR-END-ONLY PASS (`near_end_clean.wav`)

Purpose: captures the near-end speech with the cabin's ambient noise but **no echo**. Used as the near-end damage reference for ADR-0012.

1. Vehicle state unchanged.
2. **Stimulus laptop plays nothing through the head-unit.** No render.
3. Near-end speaker reads a prepared script (we recommend the [Harvard Sentences](https://www.cs.columbia.edu/~hgs/audio/harvard.html), List 1, sentences 1–10 — they're phonetically balanced and public domain). Speak naturally; don't shout.
4. Recording:
   ```sh
   ecnr_live --record-voice /tmp/<cond_id>/C_near_end.wav --duration 30
   ```
5. The captured stream contains speech + cabin ambient. **By construction**, no echo.

### Take D — FULL CONDITION (`mic.wav`)

Purpose: the production-shape recording — what the chain will actually see in deployment.

1. Vehicle state unchanged.
2. Stimulus laptop plays the same stimulus file from Take B.
3. Near-end speaker reads the SAME script as Take C, attempting the same pacing/loudness.
4. Recording captures everything: echo + near-end + ambient.
5. Recording command:
   ```sh
   ecnr_live --record-voice /tmp/<cond_id>/D_full.wav --duration <max_of_stimulus_or_30s>
   ```

The script repeat is for reproducibility. It's **not** required to be sample-aligned with Take C — that's not what the harness needs. The harness needs `near_end_clean.wav` for damage-metric measurement and `mic.wav` for the real-chain run; they don't need to share a timeline.

## 5. Immediate post-take verification (in the vehicle, before moving on)

For each condition, before driving to the next:

1. **Listen back to all 4 takes** through the operator's headphones at a comfortable level. Confirm:
   - A is silent except for ambient noise (no near-end speech, no click).
   - B has clearly audible echo of the stimulus but no near-end speech.
   - C has clearly audible near-end speech with ambient floor but no echo.
   - D has both echo and near-end.
2. **Compute quick RMS** on each. The expected ordering: `RMS(A) < RMS(C) ≈ RMS(B)` (ambient < near-end ≈ echo) and `RMS(D) > both B and C`. If the ordering is wrong (e.g., B is quieter than A), something is broken — redo.
3. **Mark the condition complete** in the session log only after all 4 takes pass verification.

A bad condition discovered later costs an entire vehicle re-booking. The 5 minutes spent verifying in the vehicle is the highest-value 5 minutes of the session.

## 6. Per-condition deliverables

After Phase 2 post-processing (back at the desk), each condition produces:

```
conditions/cabin_phase2/<cond_id>/
  mic.wav             # = Take D, trimmed to match ref.wav length
  ref.wav             # = the stimulus file (the same across all conditions)
  echo_only_mic.wav   # = Take B, trimmed to match ref.wav length
  near_end_clean.wav  # = Take C, trimmed to a fixed duration (e.g., 20 s)
  ambient.wav         # = Take A, kept for ambient-noise analysis
  meta.toml           # vehicle ID, speed, HVAC, windows, music, SPL notch, render-tap delay
```

Plus a top-level `conditions/cabin_phase2/MANIFEST.tsv` per the [ADR-0011 §3](adr/0011-aec3-tuning-methodology.md) layout, with sha256 of each WAV.

## 7. Post-session processing (back at the desk)

In rough order:

1. **Transfer the raw takes** from the recording laptop to the desk machine. Original raw takes go to long-term storage; never edit them.
2. **Resample if needed.** Everything ends up at 16 kHz mono int16 for the chain. Use `sox` or `ffmpeg`.
3. **Trim each Take to the canonical condition length.** Take B and Take D are trimmed to the stimulus length; Take C is trimmed to 20 s.
4. **Time-align Take B and Take D to `ref.wav`** using the render-tap delay measured in §2.3. The `mic.wav` and `echo_only_mic.wav` should both start at the moment the first stimulus sample plays at the speaker (i.e., delay-compensated).
5. **Compute sanity-check metrics** before declaring the condition usable:
   - On `echo_only_mic.wav`, compute RMS_echo and compare to `ambient.wav` RMS_ambient. SNR should be > 15 dB; below that means the stimulus didn't actually come through.
   - Run `ecnr_eval --run --conditions conditions/cabin_phase2 --out /tmp/sanity.csv`. The reported ERLE should be positive. If it's negative, the time alignment is wrong — fix and redo.
6. **Write `meta.toml`** per condition with all the §3 matrix values + the render-tap delay + the calibration notch.
7. **Hash + manifest.** `sha256` each WAV, write `MANIFEST.tsv` rows in the existing format used by `reference/noise/MANIFEST.tsv`.
8. **Commit the manifest** (not the WAVs — they're gitignored by the `*.wav` rule). The corpus itself stays in a separate storage location indexed by the manifest's sha256s.

## 8. Common pitfalls

| Pitfall | Symptom in data | Fix |
|---|---|---|
| Render volume drifted between takes | ERLE varies wildly between same-condition takes | Lock head-unit volume to a marked notch; re-verify at each condition |
| Near-end speaker breathed into the mic during Take B | Take B has speech-like content → false near-end | Redo Take B; speaker holds breath / steps back |
| Sample rate mismatch between laptop + device | `ecnr_eval` rejects the condition at load | Confirm 16 kHz everywhere in §2.1; resample at §7 if needed |
| Bluetooth playback path | Render-tap delay > 300 ms; inconsistent across takes | Switch to aux-in / USB-C audio; Bluetooth is the enemy |
| Different stimulus file between conditions | Can't compare conditions to each other | Use ONE stimulus across the whole session, pinned in MANIFEST |
| Vehicle speed drift mid-take | Mid-take noise transient ruins the condition | Cruise control if possible; redo if speed > ±5 mph from target |
| Recording laptop hit thermal throttling | mid-stream sample drops in the WAV | Plug in AC; close everything else; on macOS verify `pmset` shows AC + no thermal pressure |
| Reference mic moved between takes | Mic-position-dependent metrics drift | Tripod-mount the reference mic; don't touch it after §2 |

## 9. Session log template

Print, fill in by hand, scan + commit alongside the manifest:

```
Session date:      __________
Vehicle ID:        __________  (VIN last 6, trim, year)
Operator(s):       __________
Stimulus file:     reference/synth/phase2_stimulus.wav  sha256:__________
Stimulus duration: ____ s
Calibrated SPL:    ____ dBA at driver-head, head-unit notch ____
Render-tap delay:  ____ ms (measured in §2.3)
Anechoic baseline: present? Y / N

Per-condition log:
  c01_idle_quiet      A:__ B:__ C:__ D:__   notes:__________
  c02_idle_hvac       A:__ B:__ C:__ D:__   notes:__________
  c03_city_30mph      A:__ B:__ C:__ D:__   notes:__________
  c04_highway_65mph   A:__ B:__ C:__ D:__   notes:__________
  c05_highway_windows A:__ B:__ C:__ D:__   notes:__________
  c06_idle_music      A:__ B:__ C:__ D:__   notes:__________

(Mark each cell: OK / REDO / N/A. Take D is the production mic; if it's REDO, redo the whole condition.)
```

## 10. Going beyond the 6-condition first cut

The plan's "134 cases" total comes from expanding this 6-condition matrix along additional axes:

- **Driver / passenger speech location** (mic-azimuth × distance). Multiplies by ~3.
- **HVAC speed sweep** at idle + at highway. Multiplies by ~4.
- **Music vs voice render**, mixed in each condition. Doubles.

Phase 2 first cut commits to 24 (6 × 4 takes). Expanding to the full 134 is **Phase 2.5**, post first sanity-check that the 24-case corpus actually drives useful tuning decisions. Don't expand the corpus before the harness has shown it can produce defensible numbers on the small one.

## References

- [`docs/phase-2-cabin-characterization-plan.md`](phase-2-cabin-characterization-plan.md) — the "what data we need" companion plan
- [ADR-0011 §3](adr/0011-aec3-tuning-methodology.md) — the 4-track condition layout this protocol produces
- [ADR-0002](adr/0002-reverb-tail-strategy.md), [ADR-0009](adr/0009-media-aware-aec.md), [ADR-0010](adr/0010-mic-geometry-and-beamforming.md) — downstream consumers of the corpus
- ITU-T P.1100 / P.1110 — handsfree-in-vehicle acoustic test profile (industry standard for conditions)
- 3GPP TS 26.131 / 26.132 — terminal acoustic spec + test methodology
