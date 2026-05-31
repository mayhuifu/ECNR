# AEC-Challenge Integration — Design Spec

| | |
|---|---|
| **Date** | 2026-05-31 |
| **Status** | Approved (brainstorming complete) → ready for implementation plan |
| **Related ADRs** | [ADR-0011](../../adr/0011-aec3-tuning-methodology.md) (eval methodology), [ADR-0012](../../adr/0012-phase-1-acceptance-bar.md) (acceptance bar) |
| **Supersedes** | None |
| **Affected files** | `reference/fetch_aec_challenge.py` (new), `reference/run_aec_challenge.py` (new), `reference/score_mos.py` (modify), `.gitignore` (modify), `datasets/aec_challenge/MANIFEST.tsv` (new), `docs/phase-1-acceptance-grade-aec-challenge.md` (new, post-first-run) |

## Problem

The Phase-1 perceptual gate (ADR-0012) is currently graded against two ad-hoc synthetic fixtures: `reference/mixed_sound.wav` + `reference/reference_sound_to_be_eliminated.wav`, and `reference/synth/test_mic_road.wav`. Both have been flagged by the user as "not properly done" — the synthetic mixes don't represent real device captures and the resulting MOS numbers don't generalise. We need a standardised, defensible test corpus to grade the chain against.

## Goal

Integrate a 30-clip stratified subset of the [Microsoft AEC-Challenge](https://github.com/microsoft/AEC-Challenge) real-recordings test set as a reproducible perceptual-quality test corpus, and produce a first-run diagnostic report of where the current chain lands on it.

## Non-goals

- **No ADR-0012 update in this pass.** First run is purely diagnostic. Floor/target recalibration is a follow-up decision once we've seen the spread.
- **No `ecnr_eval` schema change.** AEC-Challenge real clips have no silence-pass `echo_only_mic.wav`; rather than extend the conditions tree to make the oracle optional, we bypass `ecnr_eval` entirely and run the chain through `ecnr_bench` directly.
- **No NS-blend or chain sweep in this pass.** Single chain config per invocation. Multi-config sweep against the corpus is a follow-up (Approach C from brainstorming, deferred).
- **No CI wiring.** Manual reproduction via a documented command. CI integration follows once the gate is recalibrated.
- **No SI-SDR / PESQ / true-ERLE columns.** AEC-Challenge real recordings don't include the ground-truth clean speech these metrics require; only perceptual MOS (AECMOS + DNSMOS).

## Decisions made during brainstorming

| # | Decision | Rationale |
|---|---|---|
| 1 | **30-clip stratified subset**, 10 each of `doubletalk` / `nearend_singletalk` / `farend_singletalk` | Even split is the defensible default; doubletalk-weighted alternative rejected as premature optimisation. ~5 min audio total per invocation → <10 min wall-time end-to-end |
| 2 | **Bypass `ecnr_eval`; perceptual scores only** | AEC-Challenge is graded on AECMOS, not ERLE; the real-recordings test set has no silence-pass for true-ERLE; matches what the challenge actually measures |
| 3 | **Collect numbers first; recalibrate ADR-0012 later** | First run is diagnostic. Lowest risk of locking in wrong floor/target values |
| 4 | **MANIFEST.tsv is committed; WAVs are not** | Mirrors the existing `vendor/MANIFEST.tsv` and `reference/noise/MANIFEST.tsv` patterns |
| 5 | **Two-mode fetcher (`--bootstrap` vs default validate-and-fetch)** | Removes manual SHA256-computation step; first developer bootstraps and commits the manifest; everyone after gets bit-identical clips via the default mode |
| 6 | **Single config per invocation** | Matches `ecnr_bench` philosophy; AGC-on/off and any future config comparison runs in two separate `--out-dir`s |
| 7 | **Score `talk_type` per scenario, NaN the irrelevant columns** | CSV stays rectangular; ST_NE has no echo, ST_FE has no near-end — scoring the irrelevant metric produces meaningless numbers, NaN is honest |
| 8 | **Per-clip CSV includes `status` column** | Failed clips are debuggable instead of silently missing |

## Architecture

```
ECNR/
├── reference/
│   ├── fetch_aec_challenge.py     # NEW — downloads + validates the 30-clip subset
│   ├── run_aec_challenge.py       # NEW — runs ecnr_bench × 30, scores, aggregates
│   ├── score_mos.py               # MODIFY — add talk_type-aware AECMOS dispatcher
│   ├── check_acceptance_bar.py    # UNCHANGED (ADR-0012 untouched per decision 3)
│   └── sweep_ns_blend.py          # UNCHANGED (deferred sweep extension)
├── datasets/
│   └── aec_challenge/
│       ├── MANIFEST.tsv           # COMMITTED — defines the subset
│       ├── doubletalk/            # gitignored *.wav
│       ├── nearend_singletalk/    # gitignored *.wav
│       └── farend_singletalk/     # gitignored *.wav
├── .gitignore                     # MODIFY — allow datasets/**/MANIFEST.tsv
└── docs/
    └── phase-1-acceptance-grade-aec-challenge.md  # NEW (after first run)
```

## Subset & manifest mechanics

**Source of truth = `datasets/aec_challenge/MANIFEST.tsv`.** Schema (tab-separated):

```
clip_id      scenario              mic_filename                 ref_filename                 sha256_mic                                                          sha256_ref                                                          source_url_prefix
fe_st_001    farend_singletalk     fileid_001_mic.wav           fileid_001_lpb.wav           e3b0c44...                                                          a665a45...                                                          https://aecchallengepublic.blob.core.windows.net/aecchallenge/datasets/real/farend_singletalk/
…
```

30 rows total: 10 per scenario. Clip IDs are locked permanently — even if Microsoft rotates the upstream, the manifest tells us exactly which clips we graded against.

**`fetch_aec_challenge.py` modes:**

1. **Default (validate-and-fetch).** Read MANIFEST.tsv. For each row, check if `<datasets-root>/<scenario>/<filename>` exists with matching SHA256 → skip. Else download from `{source_url_prefix}{filename}`, validate SHA256, place. Idempotent.

2. **`--bootstrap`.** Consumes a `CLIP_LIST` constant in the script (`(scenario, clip_id, mic_filename, ref_filename)` tuples). Downloads from the AEC-Challenge Azure URLs, *computes* the SHA256s, *writes* MANIFEST.tsv. Used exactly once; results committed.

**Scenario mapping** (used for AECMOS `talk_type`):

| Folder | Scenario tag | AECMOS `talk_type` | Meaningful AECMOS columns |
|---|---|---|---|
| `doubletalk` | `dt` | `dt` | echo, other, dt |
| `nearend_singletalk` | `st_ne` | `st_ne` | other only — no echo to suppress |
| `farend_singletalk` | `st_fe` | `st_fe` | echo only — no near-end to damage |

DNSMOS P.835 applies uniformly to all 30 clips regardless of scenario.

## Run flow

### CLI

```sh
python3 reference/run_aec_challenge.py \
    --bench ./build/ecnr_bench \
    --manifest datasets/aec_challenge/MANIFEST.tsv \
    --datasets-root datasets/aec_challenge \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/aec_challenge_run \
    [--agc]                                    # default off
    [--bench-flags "--ns-vad-blend 0.0,0.5"]   # passthrough to ecnr_bench
    [--keep-enh-wavs / --no-keep-enh-wavs]     # default keep
```

### Per-clip processing

For each row in MANIFEST.tsv:

1. **Verify cache hit.** `<datasets-root>/<scenario>/<filename>` exists AND SHA256 matches. Miss → exit 2 with "run `fetch_aec_challenge.py` first."
2. **Run chain.** `./build/ecnr_bench --mic <mic> --ref <ref> --out <out-dir>/<clip_id>_enh.wav --bypass-beamformer [--agc] [bench-flags…]`. Capture `cpu_ms_per_frame`, `rtf`, `erle_reported_db` from stdout's last line. `--bypass-beamformer` is always passed (AEC-Challenge clips are single-mic), matching the convention in `sweep_ns_blend.py`.
3. **Score DNSMOS P.835.** Three columns: `dnsmos_sig`, `dnsmos_bak`, `dnsmos_ovrl`.
4. **Score AECMOS** with the `talk_type` from the scenario map. Returns `(echo_mos, other_mos)`; record only the meaningful column(s), NaN the rest.
5. **Append per-clip row** to in-memory results list. Per-clip `_enh.wav` stays on disk unless `--no-keep-enh-wavs`.

### `score_mos.py` change

Minimal surgical edit:

```python
def score_aecmos(ref, mic, enh, fs, session, talk_type: str):
    """talk_type ∈ {'dt', 'st_ne', 'st_fe'} per AEC-Challenge convention."""
    ...  # same body, talk_type passed through to the model's conditioning input

# Back-compat shim for existing callers (sweep_ns_blend.py, etc.)
def score_aecmos_dt(ref, mic, enh, fs, session):
    return score_aecmos(ref, mic, enh, fs, session, talk_type='dt')
```

## Output artifacts

```
/tmp/aec_challenge_run/
├── per_clip.csv      # one row per clip
├── summary.csv       # one row per (scenario, metric) — p10/p50/p90
├── README.txt        # invocation cmdline + git rev + chain config
└── <clip_id>_enh.wav # per-clip chain output (default)
```

**`per_clip.csv` columns:**

`clip_id, scenario, dnsmos_sig, dnsmos_bak, dnsmos_ovrl, aecmos_echo, aecmos_other, aecmos_dt, erle_reported_db, cpu_ms_per_frame, rtf, status`

`status ∈ {ok, bench_failed, score_failed}` — failed clips visible in the CSV, not hidden.

**`summary.csv` columns** (long-format):

`scenario, metric, n_clips, p10, p50, p90, n_below_floor, n_below_target`

ST_NE rows omit `aecmos_echo`; ST_FE rows omit `aecmos_other`; DT keeps all three AECMOS metrics.

### Stdout summary

Per-scenario ANSI-coloured table:

```
═══ doubletalk ═══   (10 clips, 10 ok, 0 failed)
  metric           p10    p50    p90   floor   target   verdict
  dnsmos_sig      2.41   2.78   3.12    3.0      3.5    7 below floor
  dnsmos_bak      3.55   3.81   4.05    2.5      3.0    ✓
  dnsmos_ovrl     2.10   2.45   2.78    2.7      3.0    8 below floor
  aecmos_echo     3.92   4.21   4.48    3.5      4.0    ✓
  aecmos_other    1.82   2.04   2.35    —        —      (informational)
  aecmos_dt       2.87   3.12   3.41    3.0      3.5    3 below floor
```

Final one-line **CORPUS VERDICT**: PASS only if every scenario's `p50` clears every applicable floor.

## Error handling

| Failure | Behavior |
|---|---|
| Cache miss | Exit 2, list missing clips, suggest `fetch_aec_challenge.py` |
| `ecnr_bench` non-zero | Log to stderr, write `status=bench_failed`, NaN scores, continue |
| Score throws (audio too short, model error) | Log, `status=score_failed`, NaN, continue |
| ≥3 failures in any one scenario | Print loud WARNING after that scenario's block |
| All 10 clips of a scenario fail | Exit 1 at end, after still writing CSVs and other scenarios |
| SHA mismatch on cached file | Exit 2 — points to upstream rotation or local corruption |

## Testing

No new unit tests (matches `reference/` conventions). Verification by:

- `--bootstrap`-then-fetch-then-run on a 1-clip-per-scenario stub manifest (3 clips total) to smoke-test the pipeline end-to-end before running against the real 30
- Spot-check one enhanced WAV with `afplay` to confirm chain ran and didn't write silence
- Verify `aecmos_dt` for a DT scenario clip falls within ±0.1 of an `aecmos.score` call from the reference Microsoft repo on the same clip (one-time sanity check, not committed)

## Open implementation-phase tasks

These are deliberately not pinned at design time — they require live information that may have changed since this spec was written:

1. **Lock the AEC-Challenge URL pattern.** `WebFetch` `microsoft/AEC-Challenge`'s current `download_aec_challenge_real_recordings.sh` (or current equivalent) to confirm Azure blob URLs and filename conventions. Update `CLIP_LIST` in `fetch_aec_challenge.py` accordingly.
2. **Lock the 30 specific clip IDs.** Pick the first 10 alphabetically from each scenario folder, OR if the published list is small enough, document why this specific 30.
3. **Bootstrap & commit the manifest.** Run `fetch_aec_challenge.py --bootstrap`, commit the generated MANIFEST.tsv. This is the load-bearing moment that locks the subset.
4. **First diagnostic run.** Run with default `--agc` off + `--bypass-beamformer`. Write `docs/phase-1-acceptance-grade-aec-challenge.md` with the numbers + observations.

## Reproduction (post-implementation)

```sh
# One-time: fetch the 30 clips
python3 reference/fetch_aec_challenge.py

# Run current chain against the corpus
mkdir -p /tmp/aec_challenge_run
python3 reference/run_aec_challenge.py \
    --bench ./build/ecnr_bench \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/aec_challenge_run
```

## References

- [Microsoft AEC-Challenge GitHub](https://github.com/microsoft/AEC-Challenge) — corpus + AECMOS model source
- [ADR-0011](../../adr/0011-aec3-tuning-methodology.md) — the existing eval methodology (we're sitting alongside, not replacing)
- [ADR-0012](../../adr/0012-phase-1-acceptance-bar.md) — the acceptance bar this corpus will eventually feed back into
- [`docs/phase-1-acceptance-grade.md`](../../phase-1-acceptance-grade.md) — the previous first-run grade against the rejected ad-hoc fixtures
