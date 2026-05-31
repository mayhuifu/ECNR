# AEC-Challenge Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire Microsoft's AEC-Challenge real-recordings test set (30-clip stratified subset) into the ECNR project as the perceptual-quality test corpus, produce a first diagnostic grade against the current chain.

**Architecture:** Two new Python scripts under `reference/` — `fetch_aec_challenge.py` (downloads + SHA-validates the subset against a committed `MANIFEST.tsv`) and `run_aec_challenge.py` (iterates the manifest, runs `ecnr_bench` per clip, scores with `talk_type`-aware AECMOS + DNSMOS, aggregates per-scenario percentiles). One small extension to `reference/score_mos.py` for `talk_type`-aware AECMOS. No `ecnr_eval` changes.

**Tech Stack:** Python 3 (no new deps — reuses existing `numpy`, `scipy`, `onnxruntime`, `librosa`). C++ build is unchanged. `aria2c` or `curl` for downloads.

**Spec:** [docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md](../specs/2026-05-31-aec-challenge-integration-design.md) (commit `9355df3`).

**Project conventions:**
- No tests under `reference/` (matches existing pattern — none of the scripts there have unit tests). Verification is by smoke-running the script with controlled input and checking output.
- Atomic commits per chunk.
- Throwaway verification scripts go under `/tmp/`, never committed.

---

## Phase 0 — Reconnaissance (no code committed)

### Task 0.1: Lock the AEC-Challenge URL pattern + filename convention

**Files:**
- Create: `/tmp/aec_recon.md` (uncommitted notes — agent's working memory)

- [ ] **Step 1: Fetch the AEC-Challenge README**

```
WebFetch https://github.com/microsoft/AEC-Challenge with prompt:
"Find the download script for the real-recordings test set
 (often called real_test_set or AEC-Challenge real recordings).
 Report: the exact URL pattern (Azure blob storage or other), the
 filename convention for mic/lpb pairs, and whether the dataset
 is distributed as a single zip or per-file."
```

- [ ] **Step 2: If WebFetch finds a download script, follow it**

```
WebFetch the download script URL identified in Step 1 to read its contents.
Locate: the BASE_URL constant, the per-scenario folder structure
(doubletalk / nearend_singletalk / farend_singletalk), and the
mic/lpb filename pattern.
```

- [ ] **Step 3: Write recon notes to /tmp/aec_recon.md**

Capture three things:
1. `URL_PREFIX_PATTERN` — e.g., `https://aecchallengepublic.blob.core.windows.net/aecchallenge/datasets/real/{scenario}/` (the actual value from the download script)
2. Filename pattern — e.g., `<clip_id>_mic.wav` + `<clip_id>_lpb.wav`
3. Distribution shape — per-file (preferred) vs zip (requires Task 2.1 adjustment to download+extract once vs per-file)

Expected: a 10-line markdown file with these three locked.

- [ ] **Step 4: Confirm exit criteria before proceeding**

Verify the recon notes file answers all three. If WebFetch couldn't find the script, fall back: `WebFetch https://github.com/microsoft/AEC-Challenge/blob/main/README.md` and look for "Datasets" or "Download" section. If still not found, escalate to user before proceeding to Task 0.2.

**No commit. Recon notes stay in /tmp.**

---

### Task 0.2: Pick the 30 clip IDs (10 per scenario)

**Files:**
- Modify: `/tmp/aec_recon.md` (append clip lists)

- [ ] **Step 1: Identify the clip-listing source**

The AEC-Challenge repo typically ships either a directory listing or a CSV/manifest of all clips in each scenario. Locate it via:
```
WebFetch https://github.com/microsoft/AEC-Challenge/tree/main/datasets
or check what the download script downloads — sometimes it iterates a known per-scenario count.
```

- [ ] **Step 2: Pick 10 clip IDs per scenario, alphabetical**

Selection rule: **first 10 alphabetically** from each of the three scenarios. This is reproducible (anyone can replay), defensible (no cherry-picking), and matches the spec's "even split" decision.

Append to `/tmp/aec_recon.md`:
```
## CLIP_LIST (30 entries)

doubletalk:
  - dt_001 (filename: ...)
  - dt_002
  ...
nearend_singletalk:
  ...
farend_singletalk:
  ...
```

- [ ] **Step 3: Sanity check the picks**

Each clip should be ~10 s @ 16 kHz mono per the AEC-Challenge spec. If any clip's metadata suggests it's much longer/shorter or a different sample rate, drop it and use the 11th alphabetical instead.

**No commit. Recon notes stay in /tmp.**

---

### Task 0.3: Lock the AECMOS talk_type→scenario-marker mapping

**Files:**
- Modify: `/tmp/aec_recon.md` (append talk_type mapping)

- [ ] **Step 1: Fetch the AECMOS reference implementation**

```
WebFetch https://github.com/microsoft/AEC-Challenge/blob/main/AECMOS/AECMOS_local/aecmos.py with prompt:
"Find how the talk_type parameter ('dt', 'st_ne' or 'nearend', 'st_fe' or 'farend')
 maps to the scenario marker that gets appended to the mel-spectrogram features.
 The marker is two 20-row planes. Report the exact (plane1_value, plane2_value)
 for each of 'dt', 'st_ne', 'st_fe'. Show the relevant code snippet."
```

- [ ] **Step 2: Cross-check against the existing dt encoding in score_mos.py**

Open `reference/score_mos.py:230-241`. The existing code for 'dt' appends `(ones, zeros)` planes. Confirm this matches what Microsoft's reference does for `talk_type='dt'`. If there's a discrepancy, the existing code is the authoritative source for 'dt' (it's working in production) — note the discrepancy in /tmp/aec_recon.md for review.

- [ ] **Step 3: Lock the mapping table in /tmp/aec_recon.md**

Append:
```
## AECMOS talk_type → scenario marker

talk_type='dt'    → (plane1=<all_ones_or_zeros>, plane2=<...>)  # from existing code
talk_type='st_ne' → (plane1=<...>, plane2=<...>)               # from reference
talk_type='st_fe' → (plane1=<...>, plane2=<...>)               # from reference
```

**No commit. Recon notes stay in /tmp.**

---

## Phase 1 — `score_mos.py` extension

### Task 1.1: Add talk_type-aware score_aecmos

**Files:**
- Modify: `reference/score_mos.py:209-253` (generalize `score_aecmos_dt`)

- [ ] **Step 1: Rewrite score_aecmos_dt as score_aecmos + back-compat shim**

Replace lines 209-253 with the code below. The scenario-marker construction is **per-stream** (mic/lpb/enh each get different plane-1 values per `talk_type`) — verified against `microsoft/AEC-Challenge/AECMOS/AECMOS_local/aecmos.py` in `/tmp/aec_recon.md` Task 0.3.

```python
# Public talk_type names → Microsoft AECMOS internal names. We keep our
# clearer mnemonics on the public surface (per_clip.csv etc.) and translate
# at this boundary. Source: AECMOS/AECMOS_local/aecmos.py asserts
# talk_type ∈ {'nst','st','dt'}.
_TALK_TYPE_TO_MS = {"st_ne": "nst", "st_fe": "st", "dt": "dt"}


def score_aecmos(lpb: np.ndarray, mic: np.ndarray, enh: np.ndarray,
                 fs: int, session: ort.InferenceSession,
                 talk_type: str = "dt") -> tuple[float, float, float]:
    """Run AECMOS under the given talk_type.

    talk_type ∈ {'dt', 'st_ne', 'st_fe'} per AEC-Challenge convention.
    Returns (echo_mos, other_mos, dt_combined). For single-talk scenarios
    only one of echo_mos / other_mos is meaningful; the caller is
    responsible for NaN-ing the irrelevant column.
    """
    if talk_type not in _TALK_TYPE_TO_MS:
        raise ValueError(f"talk_type must be one of "
                         f"{list(_TALK_TYPE_TO_MS)}, got {talk_type!r}")
    ms = _TALK_TYPE_TO_MS[talk_type]
    # Derive the (ne_st, fe_st) flags per upstream:
    #   nst → ne_st=1, fe_st=0
    #   st  → ne_st=0, fe_st=1
    #   dt  → ne_st=0, fe_st=0
    ne_st = 1 if ms == "nst" else 0
    fe_st = 1 if ms == "st"  else 0

    # Resample to 16 kHz if needed.
    if fs != AECMOS_SR:
        lpb = _resample_to_16k(lpb, fs)
        mic = _resample_to_16k(mic, fs)
        enh = _resample_to_16k(enh, fs)
    # Match lengths to the shortest signal.
    n = min(len(lpb), len(mic), len(enh))
    lpb, mic, enh = lpb[:n], mic[:n], enh[:n]
    # Clip to max-len if too long.
    n_cap = AECMOS_MAX_LEN_S * AECMOS_SR
    if n > n_cap:
        lpb, mic, enh = lpb[:n_cap], mic[:n_cap], enh[:n_cap]

    # Mel features.
    lpb_f = _aecmos_mel_transform(lpb)
    mic_f = _aecmos_mel_transform(mic)
    enh_f = _aecmos_mel_transform(enh)

    # Scenario marker: two 20-row tail planes per stream. Plane 2 always zeros.
    # Plane 1 differs per stream:
    #   mic:  (1 - fe_st)   → 1 for dt/st_ne, 0 for st_fe
    #   lpb:  (1 - ne_st)   → 1 for dt/st_fe, 0 for st_ne
    #   enh:  1.0           → always
    n_mels = mic_f.shape[1]
    zeros = np.zeros((20, n_mels), dtype=np.float32)
    def _append(x, plane1_value: float):
        plane1 = np.full((20, n_mels), plane1_value, dtype=np.float32)
        return np.concatenate((x, plane1, zeros), axis=0)
    mic_f = _append(mic_f, 1.0 - fe_st)
    lpb_f = _append(lpb_f, 1.0 - ne_st)
    enh_f = _append(enh_f, 1.0)

    # Stack channel-first: (1, 3, n_time, n_mels).
    feats = np.stack((lpb_f, mic_f, enh_f)).astype(np.float32)
    feats = np.expand_dims(feats, axis=0)
    h0 = np.zeros(AECMOS_HIDDEN, dtype=np.float32)

    in_name = session.get_inputs()[0].name
    result = session.run([], {in_name: feats, "h0": h0})
    echo_mos = float(result[0][0])
    deg_mos = float(result[0][1])
    dt_combined = 0.5 * (echo_mos + deg_mos)
    return echo_mos, deg_mos, dt_combined


def score_aecmos_dt(lpb: np.ndarray, mic: np.ndarray, enh: np.ndarray,
                     fs: int, session: ort.InferenceSession) -> tuple[float, float, float]:
    """Back-compat shim. Use score_aecmos(..., talk_type='dt') in new code."""
    return score_aecmos(lpb, mic, enh, fs, session, talk_type="dt")
```

- [ ] **Step 2: Verify parity — old code path still produces identical numbers**

Create `/tmp/verify_score_aecmos.py`:

```python
"""Verify score_aecmos(talk_type='dt') matches score_aecmos_dt on real audio."""
import sys; sys.path.insert(0, 'reference')
import numpy as np
import onnxruntime as ort
from score_mos import score_aecmos, score_aecmos_dt, _read_mono_float

sess = ort.InferenceSession('models/aecmos.onnx',
                             providers=['CPUExecutionProvider'])

# Use existing fixtures.
mic, fs = _read_mono_float('reference/mixed_sound.wav')
ref, _ = _read_mono_float('reference/reference_sound_to_be_eliminated.wav')
# enh = same as mic for a no-op smoke test (we're checking encoding parity, not chain output)
enh = mic.copy()

old = score_aecmos_dt(ref, mic, enh, fs, sess)
new = score_aecmos(ref, mic, enh, fs, sess, talk_type='dt')
print(f"old dt: {old}")
print(f"new dt: {new}")
assert all(abs(a - b) < 1e-5 for a, b in zip(old, new)), "DT parity broken"
print("OK: dt parity preserved")

# Also smoke-test that st_ne and st_fe don't crash.
for tt in ['st_ne', 'st_fe']:
    r = score_aecmos(ref, mic, enh, fs, sess, talk_type=tt)
    print(f"{tt}: {r}")
    assert all(1.0 <= v <= 5.0 for v in r), f"{tt} out of MOS range"
print("OK: st_ne and st_fe run without crash, results in MOS range")
```

Run: `python3 /tmp/verify_score_aecmos.py`
Expected output:
```
old dt: (e, o, dt)
new dt: (e, o, dt)     # identical to old
OK: dt parity preserved
st_ne: (e, o, dt)
st_fe: (e, o, dt)
OK: st_ne and st_fe run without crash, results in MOS range
```

If parity assertion fails: the new code's encoding for 'dt' diverges from the old. Re-verify the `_AECMOS_SCENARIO_MARKER['dt']` value and the `_append_marker` logic.

- [ ] **Step 3: Verify back-compat callers still work**

The shim `score_aecmos_dt` is called from `score_mos.py:355` (the main `--in-csv` path) and from `sweep_ns_blend.py:127`. Smoke-test the latter:

```bash
python3 reference/sweep_ns_blend.py \
    --mic reference/mixed_sound.wav \
    --ref reference/reference_sound_to_be_eliminated.wav \
    --bench ./build/ecnr_bench \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-csv /tmp/parity_sweep.csv 2>&1 | tail -20
```

Expected: same per-config table as before the change (numbers within ±0.01 — ONNX nondeterminism floor). If numbers drift more than 0.01, the back-compat shim is broken.

- [ ] **Step 4: Commit**

```bash
git add reference/score_mos.py
git commit -m "$(cat <<'EOF'
refactor(score_mos): generalize score_aecmos for arbitrary talk_type

Replaces hard-coded talk_type='dt' encoding with a (plane1, plane2)
table indexed by talk_type ∈ {'dt', 'st_ne', 'st_fe'} per the AECMOS
reference. score_aecmos_dt survives as a back-compat shim so
sweep_ns_blend.py and the existing --in-csv path keep working.

Prep work for the AEC-Challenge integration (per
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md).
DT parity verified against the old code path; ST_NE / ST_FE smoke-tested
for non-crash + in-range MOS output.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 2 — Fetcher

### Task 2.1: Write fetch_aec_challenge.py scaffolding + CLIP_LIST

**Files:**
- Create: `reference/fetch_aec_challenge.py`

- [ ] **Step 1: Write the script skeleton**

Local layout is **flat** (mirrors upstream — see spec Addendum A1). MANIFEST.tsv has six columns (no `source_url_prefix` — that's a script constant). ST_NE clips have **no upstream lpb file**; the fetcher synthesizes a zero-filled lpb WAV to keep the schema rectangular (spec Addendum A2).

```python
#!/usr/bin/env python3
"""Fetch the AEC-Challenge real-recordings 30-clip subset.

Modes:
  --bootstrap   Download CLIP_LIST entries from upstream, compute SHA256s,
                write MANIFEST.tsv. Used exactly once to seed the manifest.
  (default)     Read existing MANIFEST.tsv. For each row, check if local
                file exists with matching SHA256; download if not.

Output tree (flat — mirrors upstream layout per spec Addendum A1):
  datasets/aec_challenge/
    MANIFEST.tsv                                            (committed; defines the subset)
    <GUID>_<scenario>_<mic|lpb>.wav                         (gitignored)
    <GUID>_nearend_singletalk_lpb_silence.wav               (gitignored; synthesized — no upstream lpb)

Per the design spec at
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
import sys
import urllib.request
import wave
from pathlib import Path

# GitHub LFS-media endpoint — locked in Task 0.1 against the AEC-Challenge
# repo. The upstream tree under datasets/real/ is flat (no scenario
# subdirs); we mirror that locally. Update here if upstream rotates.
URL_PREFIX = "https://media.githubusercontent.com/media/microsoft/AEC-Challenge/main/datasets/real/"

# 30-clip subset locked in Task 0.2 — first 10 alphabetical per scenario.
# Each tuple: (scenario, clip_id, guid).
# Filenames derived: f"{guid}_{scenario}_{channel}.wav" for channel ∈ {mic, lpb}.
# nearend_singletalk has NO upstream lpb (Addendum A2) → lpb is synthesized
# as f"{guid}_nearend_singletalk_lpb_silence.wav" (zero-filled, same length as mic).
CLIP_LIST = [
    # doubletalk (10) — paste the 10 GUIDs from /tmp/aec_recon.md Task 0.2
    ("doubletalk",         "dt_01", "-2jLGNCgf0WDpKMY2iup7g"),
    ("doubletalk",         "dt_02", "-3Jxai1udE6V32_c-aUJFA"),
    # ... 8 more from recon notes ...

    # farend_singletalk (10)
    ("farend_singletalk",  "fe_01", "-0AcvGNEdEK-DQGxWmtq2Q"),
    # ... 9 more from recon notes ...

    # nearend_singletalk (10) — lpb is synthesized
    ("nearend_singletalk", "ne_01", "-0AcvGNEdEK-DQGxWmtq2Q"),
    # ... 9 more from recon notes ...
]

MANIFEST_COLS = ["clip_id", "scenario", "mic_filename", "ref_filename",
                 "sha256_mic", "sha256_ref"]

SR_HZ = 16000  # AEC-Challenge clips are 16 kHz mono 16-bit


def mic_filename(scenario: str, guid: str) -> str:
    return f"{guid}_{scenario}_mic.wav"


def ref_filename(scenario: str, guid: str) -> str:
    if scenario == "nearend_singletalk":
        return f"{guid}_nearend_singletalk_lpb_silence.wav"
    return f"{guid}_{scenario}_lpb.wav"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path) -> None:
    """Download url → dest. Atomic via .part swap."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
            while True:
                chunk = r.read(1 << 16)
                if not chunk: break
                f.write(chunk)
        tmp.rename(dest)
    except Exception:
        if tmp.exists(): tmp.unlink()
        raise


def write_silence_wav(dest: Path, n_samples: int) -> None:
    """Write a zero-filled int16 mono 16 kHz WAV. Atomic via .part swap."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    try:
        with wave.open(str(tmp), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR_HZ)
            w.writeframes(b"\x00\x00" * n_samples)
        tmp.rename(dest)
    except Exception:
        if tmp.exists(): tmp.unlink()
        raise


def _wav_n_samples(path: Path) -> int:
    with wave.open(str(path), "rb") as w:
        return w.getnframes()


def bootstrap(out_root: Path, manifest_path: Path) -> int:
    """Download all CLIP_LIST entries, synthesize ST_NE silence,
    compute SHAs, write MANIFEST.tsv."""
    rows = []
    for scenario, clip_id, guid in CLIP_LIST:
        mic_fn = mic_filename(scenario, guid)
        ref_fn = ref_filename(scenario, guid)
        mic_dest = out_root / mic_fn
        ref_dest = out_root / ref_fn

        # mic: always a real download
        if not mic_dest.exists():
            print(f"  fetching {mic_fn} ...")
            download(URL_PREFIX + mic_fn, mic_dest)
        else:
            print(f"  cached   {mic_fn}")

        # ref: download from upstream OR synthesize silence for nearend_singletalk
        if scenario == "nearend_singletalk":
            if not ref_dest.exists():
                n = _wav_n_samples(mic_dest)
                print(f"  synth    {ref_fn}  ({n} samples = {n/SR_HZ:.2f}s silence)")
                write_silence_wav(ref_dest, n)
            else:
                print(f"  cached   {ref_fn}  (synth)")
        else:
            if not ref_dest.exists():
                print(f"  fetching {ref_fn} ...")
                download(URL_PREFIX + ref_fn, ref_dest)
            else:
                print(f"  cached   {ref_fn}")

        rows.append({
            "clip_id": clip_id,
            "scenario": scenario,
            "mic_filename": mic_fn,
            "ref_filename": ref_fn,
            "sha256_mic": sha256_of(mic_dest),
            "sha256_ref": sha256_of(ref_dest),
        })
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    with open(manifest_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=MANIFEST_COLS, delimiter="\t")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"\nWrote {manifest_path} with {len(rows)} entries.")
    return 0


def validate_and_fetch(out_root: Path, manifest_path: Path) -> int:
    """Read MANIFEST.tsv. For each row, verify-or-fetch each WAV.

    For nearend_singletalk rows, the lpb file is regenerated as silence
    (matching mic length) if missing or SHA-mismatched."""
    if not manifest_path.exists():
        print(f"MANIFEST.tsv not found at {manifest_path} — run with --bootstrap first",
              file=sys.stderr)
        return 2
    n_ok = n_fetched = n_failed = 0
    with open(manifest_path) as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            scenario = row["scenario"]
            for which in ("mic", "ref"):
                fn = row[f"{which}_filename"]
                expected_sha = row[f"sha256_{which}"]
                dest = out_root / fn
                if dest.exists() and sha256_of(dest) == expected_sha:
                    n_ok += 1
                    continue
                if dest.exists():
                    print(f"  SHA mismatch: {fn} — re-acquiring", file=sys.stderr)
                    dest.unlink()
                # Synthesize silence for ST_NE lpb; download everything else.
                is_synth = (which == "ref" and scenario == "nearend_singletalk")
                try:
                    if is_synth:
                        mic_dest = out_root / row["mic_filename"]
                        if not mic_dest.exists() or sha256_of(mic_dest) != row["sha256_mic"]:
                            print(f"  ERROR: cannot synth {fn} — mic not present/valid yet",
                                  file=sys.stderr)
                            n_failed += 1
                            continue
                        n = _wav_n_samples(mic_dest)
                        print(f"  synth    {fn}  ({n} samples)")
                        write_silence_wav(dest, n)
                    else:
                        print(f"  fetching {fn} ...")
                        download(URL_PREFIX + fn, dest)
                    if sha256_of(dest) != expected_sha:
                        print(f"  ERROR: acquired file has wrong SHA256: {fn}",
                              file=sys.stderr)
                        n_failed += 1
                        continue
                    n_fetched += 1
                except Exception as e:
                    print(f"  ERROR: acquire failed: {fn}: {e}", file=sys.stderr)
                    n_failed += 1
    print(f"\nok={n_ok}  acquired={n_fetched}  failed={n_failed}")
    return 0 if n_failed == 0 else 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bootstrap", action="store_true",
                    help="Download CLIP_LIST, compute SHAs, write MANIFEST.tsv")
    ap.add_argument("--out-root", type=Path,
                    default=Path("datasets/aec_challenge"),
                    help="Local cache root")
    ap.add_argument("--manifest", type=Path, default=None,
                    help="MANIFEST.tsv path (default: <out-root>/MANIFEST.tsv)")
    args = ap.parse_args()

    manifest = args.manifest or (args.out_root / "MANIFEST.tsv")
    return bootstrap(args.out_root, manifest) if args.bootstrap \
        else validate_and_fetch(args.out_root, manifest)


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Fill in the full 30-entry CLIP_LIST from recon notes**

From `/tmp/aec_recon.md` Task 0.2, paste all 30 `(scenario, clip_id, guid)` tuples in the order: 10 `doubletalk` then 10 `farend_singletalk` then 10 `nearend_singletalk`. URL_PREFIX is already correct from recon.

- [ ] **Step 3: Sanity-check the script parses**

```bash
python3 reference/fetch_aec_challenge.py --help
```

Expected: argparse usage block prints without error.

**No commit yet — testing bootstrap mode next.**

---

### Task 2.2: Test --bootstrap with a 3-clip stub subset

**Files:**
- Modify: `reference/fetch_aec_challenge.py` (temporarily reduce CLIP_LIST to 3 entries)

- [ ] **Step 1: Save the full CLIP_LIST aside**

Copy the 30-entry CLIP_LIST to `/tmp/full_clip_list.py` (a temp backup, NOT committed). We'll restore it in Task 2.6.

- [ ] **Step 2: Reduce CLIP_LIST to 3 entries (one per scenario)**

Replace CLIP_LIST in `reference/fetch_aec_challenge.py` with just the first entry from each scenario — 3 total. This lets us validate the download/SHA/manifest flow on a small surface before committing to 30 downloads.

- [ ] **Step 3: Run --bootstrap against a stub out-root**

```bash
rm -rf /tmp/aec_stub
python3 reference/fetch_aec_challenge.py --bootstrap --out-root /tmp/aec_stub
```

Expected:
- 6 files downloaded (3 clips × mic + ref)
- `/tmp/aec_stub/MANIFEST.tsv` written with 3 rows
- `ok=0  fetched=...  failed=0` (in bootstrap mode the validate counter doesn't apply; the final print is "Wrote ... with 3 entries.")

If any download fails: revisit Task 0.1 — URL_PREFIX or filename may be wrong.

- [ ] **Step 4: Inspect the manifest**

```bash
cat /tmp/aec_stub/MANIFEST.tsv
```

Expected: 4-line TSV (header + 3 rows). Columns match `MANIFEST_COLS`. SHA256 values are 64 hex chars each. `source_url_prefix` is fully-qualified URLs.

**No commit yet — restoring 30-entry list happens in Task 2.6.**

---

### Task 2.3: Test default mode (validate-and-fetch) against the stub manifest

**Files:** (no changes — uses the stub from Task 2.2)

- [ ] **Step 1: Re-run without --bootstrap**

```bash
python3 reference/fetch_aec_challenge.py --out-root /tmp/aec_stub
```

Expected:
- All 6 files reported as "ok" (cache hit, SHA matches)
- `ok=6  fetched=0  failed=0`

- [ ] **Step 2: Delete one file, re-run, expect fetch**

```bash
rm /tmp/aec_stub/doubletalk/*_mic.wav  # one mic file
python3 reference/fetch_aec_challenge.py --out-root /tmp/aec_stub
```

Expected:
- One "fetching ..." line
- `ok=5  fetched=1  failed=0`

- [ ] **Step 3: Corrupt one file, re-run, expect SHA mismatch handling**

```bash
echo "corrupt" > /tmp/aec_stub/doubletalk/*_mic.wav  # overwrite a real file
python3 reference/fetch_aec_challenge.py --out-root /tmp/aec_stub
```

Expected:
- "SHA mismatch: ... — re-downloading"
- "fetching ..."
- `ok=5  fetched=1  failed=0` (re-download succeeds, SHA passes)

**No commit yet.**

---

### Task 2.4: Update .gitignore for MANIFEST.tsv exception

**Files:**
- Modify: `.gitignore`

- [ ] **Step 1: Find the *.wav rule**

```bash
grep -n "datasets\|\\*.wav" .gitignore
```

Expected: the `*.wav` rule appears (around the "Generated test audio" block).

- [ ] **Step 2: Add the datasets allowlist**

Insert immediately after the `!reference/**/*.md` line in `.gitignore`:

```
# AEC-Challenge subset: WAVs gitignored (caught by *.wav), MANIFEST.tsv tracked
!datasets/**/MANIFEST.tsv
```

- [ ] **Step 3: Verify the rule works**

```bash
git check-ignore -v /tmp/aec_stub/MANIFEST.tsv 2>&1 || echo "not ignored"
```

We can't directly test `datasets/aec_challenge/MANIFEST.tsv` since it doesn't exist yet, but verify the rule parses by:

```bash
git check-ignore -v -- datasets/aec_challenge/MANIFEST.tsv 2>&1
```

Expected: empty output (= NOT ignored = will be tracked).

```bash
git check-ignore -v -- datasets/aec_challenge/doubletalk/foo.wav
```

Expected: prints `.gitignore:NN:*.wav  datasets/aec_challenge/doubletalk/foo.wav` (= ignored by *.wav rule).

**No commit yet.**

---

### Task 2.5: Bootstrap on the full 30-clip list

**Files:**
- Modify: `reference/fetch_aec_challenge.py` (restore full CLIP_LIST)

- [ ] **Step 1: Restore the full 30-entry CLIP_LIST**

Replace the 3-entry stub with the 30-entry list from `/tmp/full_clip_list.py` (backup from Task 2.2 Step 1).

- [ ] **Step 2: Run --bootstrap against the real out-root**

```bash
rm -rf datasets/aec_challenge   # start clean
python3 reference/fetch_aec_challenge.py --bootstrap
```

Expected:
- 60 files downloaded (30 clips × mic + ref), each ~320 KB → total ~20 MB
- ~30-60 seconds wall time on a normal connection
- Final print: "Wrote datasets/aec_challenge/MANIFEST.tsv with 30 entries."

- [ ] **Step 3: Sanity-check the produced tree**

```bash
ls datasets/aec_challenge/
ls datasets/aec_challenge/doubletalk/ | wc -l           # → 20 (10 mic + 10 ref)
ls datasets/aec_challenge/nearend_singletalk/ | wc -l   # → 20
ls datasets/aec_challenge/farend_singletalk/ | wc -l    # → 20
wc -l datasets/aec_challenge/MANIFEST.tsv               # → 31 (header + 30)
```

- [ ] **Step 4: Re-run validate-and-fetch to confirm idempotency**

```bash
python3 reference/fetch_aec_challenge.py
```

Expected: `ok=60  fetched=0  failed=0`. Sub-second runtime.

- [ ] **Step 5: Commit**

```bash
git add reference/fetch_aec_challenge.py .gitignore datasets/aec_challenge/MANIFEST.tsv
git commit -m "$(cat <<'EOF'
feat(eval): AEC-Challenge subset fetcher + locked 30-clip manifest

reference/fetch_aec_challenge.py downloads + SHA256-validates a
30-clip stratified subset of Microsoft's AEC-Challenge real-recordings
test set. Two modes: --bootstrap (download, compute SHAs, write
MANIFEST.tsv — used once) and the default validate-and-fetch (replay
the manifest, skip cache hits, error loud on SHA drift).

Subset: 10 clips each of doubletalk / nearend_singletalk /
farend_singletalk, first-10-alphabetical per scenario for
reproducibility. MANIFEST.tsv is committed (the subset contract);
WAVs are gitignored via the existing *.wav rule; .gitignore
exempts datasets/**/MANIFEST.tsv.

Per design spec
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 3 — Runner

### Task 3.1: Write run_aec_challenge.py scaffolding (CLI + manifest reader + cache check)

**Files:**
- Create: `reference/run_aec_challenge.py`

- [ ] **Step 1: Write the script skeleton**

```python
#!/usr/bin/env python3
"""Run the ECNR chain against the AEC-Challenge 30-clip subset; produce
a perceptual-quality report.

Per-clip: ecnr_bench → AECMOS (talk_type per scenario) + DNSMOS P.835.
Aggregates per-scenario p10/p50/p90 + floor/target counts against the
ADR-0012 acceptance bar.

Per design spec
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import math
import os
import shlex
import subprocess
import sys
from pathlib import Path

try:
    import numpy as np  # type: ignore
    import onnxruntime as ort  # type: ignore
except ImportError as e:
    print(f"requires numpy + onnxruntime: {e}", file=sys.stderr)
    sys.exit(2)

# ---- Scenario → AECMOS talk_type + meaningful columns ----------------
SCENARIO_MAP = {
    "doubletalk":          {"talk_type": "dt",
                            "aecmos_cols": ("aecmos_echo", "aecmos_other", "aecmos_dt")},
    "nearend_singletalk":  {"talk_type": "st_ne",
                            "aecmos_cols": ("aecmos_other",)},
    "farend_singletalk":   {"talk_type": "st_fe",
                            "aecmos_cols": ("aecmos_echo",)},
}

# ADR-0012 §2 floors + soft targets — sourced from
# reference/check_acceptance_bar.py to stay in sync.
FLOORS = {"dnsmos_sig": 3.0, "dnsmos_bak": 2.5, "dnsmos_ovrl": 2.7,
          "aecmos_echo": 3.5, "aecmos_dt": 3.0}
TARGETS = {"dnsmos_sig": 3.5, "dnsmos_bak": 3.0, "dnsmos_ovrl": 3.0,
           "aecmos_echo": 4.0, "aecmos_dt": 3.5}

PER_CLIP_COLS = ["clip_id", "scenario",
                 "dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
                 "aecmos_echo", "aecmos_other", "aecmos_dt",
                 "erle_reported_db", "cpu_ms_per_frame", "rtf", "status"]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_score_mos():
    """Import reference/score_mos.py by path (avoids requiring a package install)."""
    spec = importlib.util.spec_from_file_location(
        "score_mos", Path(__file__).parent / "score_mos.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_manifest(path: Path) -> list[dict]:
    if not path.exists():
        print(f"MANIFEST.tsv not found at {path} — run fetch_aec_challenge.py first",
              file=sys.stderr)
        sys.exit(2)
    with open(path) as f:
        return list(csv.DictReader(f, delimiter="\t"))


def verify_cache(rows: list[dict], root: Path) -> None:
    """Exit 2 with a useful message if any clip is missing or SHA-mismatched."""
    missing = []
    for r in rows:
        for which in ("mic", "ref"):
            fn = r[f"{which}_filename"]
            dest = root / r["scenario"] / fn
            if not dest.exists() or sha256_of(dest) != r[f"sha256_{which}"]:
                missing.append(f"  {r['scenario']}/{fn}")
    if missing:
        print(f"Cache miss / SHA mismatch on {len(missing)} files. Run:\n"
              f"  python3 reference/fetch_aec_challenge.py\n\n"
              f"Missing:\n" + "\n".join(missing[:10]) +
              (f"\n  ... {len(missing) - 10} more" if len(missing) > 10 else ""),
              file=sys.stderr)
        sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", default=Path("./build/ecnr_bench"), type=Path)
    ap.add_argument("--manifest", default=Path("datasets/aec_challenge/MANIFEST.tsv"),
                    type=Path)
    ap.add_argument("--datasets-root", default=Path("datasets/aec_challenge"), type=Path)
    ap.add_argument("--dnsmos-model", required=True, type=Path)
    ap.add_argument("--aecmos-model", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--agc", action="store_true", help="pass --agc to ecnr_bench")
    ap.add_argument("--bench-flags", default="",
                    help="extra flags to pass through to ecnr_bench")
    ap.add_argument("--keep-enh-wavs", action="store_true", default=True)
    ap.add_argument("--no-keep-enh-wavs", dest="keep_enh_wavs", action="store_false")
    args = ap.parse_args()

    if not args.bench.exists():
        print(f"--bench not found: {args.bench}", file=sys.stderr); return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(args.manifest)
    verify_cache(rows, args.datasets_root)
    print(f"OK: {len(rows)} clips verified in cache")
    # Per-clip runner (Task 3.2), scoring (3.3), aggregation (3.4),
    # writers (3.5), stdout (3.6), error matrix (3.7) plug in below.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Smoke-test the scaffolding**

```bash
python3 reference/run_aec_challenge.py --help
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
```

Expected first invocation: argparse usage prints.
Expected second invocation: `OK: 30 clips verified in cache` (cache populated by Task 2.5).

- [ ] **Step 3: Verify cache-miss path**

```bash
mv datasets/aec_challenge/doubletalk/*_mic.wav /tmp/      # temporarily move
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
# Expected: "Cache miss / SHA mismatch on N files..." + exit 2
mv /tmp/*_mic.wav datasets/aec_challenge/doubletalk/      # restore
```

**No commit yet — runner is incomplete.**

---

### Task 3.2: Per-clip bench runner + stdout parser

**Files:**
- Modify: `reference/run_aec_challenge.py` (add `run_bench` + invoke in main loop)

- [ ] **Step 1: Add the bench runner function**

Insert above `main()`:

```python
def run_bench(bench: Path, mic: Path, ref: Path, out_wav: Path,
              agc: bool, extra_flags: str) -> dict:
    """Invoke ecnr_bench. Returns {erle_reported_db, cpu_ms_per_frame, rtf, status,
       stderr (on fail)}. Parses bench's last-line summary; tolerates extra columns."""
    cmd = [str(bench), "--mic", str(mic), "--ref", str(ref), "--out", str(out_wav),
           "--bypass-beamformer"]
    if agc: cmd.append("--agc")
    if extra_flags: cmd.extend(shlex.split(extra_flags))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return {"status": "bench_failed", "stderr": result.stderr,
                "erle_reported_db": math.nan,
                "cpu_ms_per_frame": math.nan, "rtf": math.nan}
    # Last non-empty stdout line is the summary, e.g.:
    # "frames=1000  audio=10.000s  cpu=0.247s  rtf=0.0247  ...  erle_db=6.87  ..."
    line = next(L for L in reversed(result.stdout.splitlines()) if L.strip())
    parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
    def _f(k, default=math.nan):
        try: return float(parts.get(k, default))
        except (ValueError, TypeError): return default
    audio_s = _f("audio".rstrip("s"), _f("audio"))
    # Bench prints "audio=10.000s" — strip trailing 's' if present.
    audio_str = parts.get("audio", "0").rstrip("s")
    try: audio_s = float(audio_str)
    except ValueError: audio_s = math.nan
    cpu_s = _f("cpu")
    # cpu_ms_per_frame = (cpu_s * 1000) / frames; bench's "frames" field
    frames = _f("frames")
    cpu_ms_per_frame = (cpu_s * 1000.0 / frames) if frames > 0 else math.nan
    return {"status": "ok",
            "erle_reported_db": _f("erle_db"),
            "cpu_ms_per_frame": cpu_ms_per_frame,
            "rtf": _f("rtf")}
```

- [ ] **Step 2: Invoke per-clip in main loop**

Replace the placeholder comment in `main()` with:

```python
    per_clip = []
    for r in rows:
        scn = r["scenario"]
        mic = args.datasets_root / scn / r["mic_filename"]
        ref = args.datasets_root / scn / r["ref_filename"]
        enh = args.out_dir / f"{r['clip_id']}_enh.wav"  # top-level per spec
        b = run_bench(args.bench, mic, ref, enh, args.agc, args.bench_flags)
        if b["status"] != "ok":
            print(f"  {r['clip_id']:<14} {scn:<22} BENCH FAILED: {b.get('stderr', '')[:80]}",
                  file=sys.stderr)
        else:
            print(f"  {r['clip_id']:<14} {scn:<22} rtf={b['rtf']:.3f}  erle={b['erle_reported_db']:.1f} dB")
        per_clip.append({**r, **b, "enh_wav": str(enh)})
    print(f"\nRan {len(per_clip)} clips, {sum(1 for c in per_clip if c['status']=='ok')} ok")
    return 0
```

- [ ] **Step 3: Smoke-test against the full corpus**

```bash
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
```

Expected:
- 30 lines like `  dt_001         doubletalk             rtf=0.025  erle=12.3 dB`
- Final: `Ran 30 clips, 30 ok`
- ~30-60 seconds total (chain is real-time on host)
- `/tmp/run_smoke/enh/*.wav` populated with 30 files

If any clip's status is bench_failed: read stderr in the failing line, fix the bench-flags/argument issue.

**No commit yet.**

---

### Task 3.3: Per-clip scoring dispatcher

**Files:**
- Modify: `reference/run_aec_challenge.py` (add `score_clip` + call after `run_bench`)

- [ ] **Step 1: Add the scoring function**

Insert above `main()`:

```python
def score_clip(clip_row: dict, enh_wav: Path,
               scoremos, dn_sess, ae_sess,
               datasets_root: Path) -> dict:
    """Score one clip; returns dict with the six MOS columns.
    NaN for irrelevant AECMOS columns per SCENARIO_MAP."""
    out = {"dnsmos_sig": math.nan, "dnsmos_bak": math.nan, "dnsmos_ovrl": math.nan,
           "aecmos_echo": math.nan, "aecmos_other": math.nan, "aecmos_dt": math.nan,
           "status": clip_row.get("status", "ok")}
    if out["status"] != "ok" or not enh_wav.exists():
        return out
    scn = clip_row["scenario"]
    mic = datasets_root / scn / clip_row["mic_filename"]
    ref = datasets_root / scn / clip_row["ref_filename"]
    try:
        enh, fs_e = scoremos._read_mono_float(enh_wav)
        mic_sig, _ = scoremos._read_mono_float(mic)
        ref_sig, _ = scoremos._read_mono_float(ref)
        # DNSMOS applies regardless of scenario.
        s, b, o = scoremos.score_dnsmos_p835(enh, fs_e, dn_sess)
        out["dnsmos_sig"], out["dnsmos_bak"], out["dnsmos_ovrl"] = s, b, o
        # AECMOS with scenario-specific talk_type.
        tt = SCENARIO_MAP[scn]["talk_type"]
        e, oth, dt = scoremos.score_aecmos(ref_sig, mic_sig, enh, fs_e, ae_sess,
                                           talk_type=tt)
        # NaN columns that don't apply to this scenario.
        cols = SCENARIO_MAP[scn]["aecmos_cols"]
        if "aecmos_echo"  in cols: out["aecmos_echo"]  = e
        if "aecmos_other" in cols: out["aecmos_other"] = oth
        if "aecmos_dt"    in cols: out["aecmos_dt"]    = dt
    except Exception as ex:
        print(f"  score_failed: {clip_row['clip_id']}: {ex}", file=sys.stderr)
        out["status"] = "score_failed"
    return out
```

- [ ] **Step 2: Wire scoring into the main loop**

Update `main()` to load ONNX sessions + score_mos module, then call `score_clip` after `run_bench`:

```python
    # ... after args parsing + verify_cache ...
    scoremos = load_score_mos()
    dn_sess = ort.InferenceSession(str(args.dnsmos_model),
                                    providers=["CPUExecutionProvider"])
    ae_sess = ort.InferenceSession(str(args.aecmos_model),
                                    providers=["CPUExecutionProvider"])

    per_clip = []
    for r in rows:
        scn = r["scenario"]
        mic = args.datasets_root / scn / r["mic_filename"]
        ref = args.datasets_root / scn / r["ref_filename"]
        enh = args.out_dir / f"{r['clip_id']}_enh.wav"  # top-level per spec
        b = run_bench(args.bench, mic, ref, enh, args.agc, args.bench_flags)
        s = score_clip({**r, "status": b["status"]}, enh,
                        scoremos, dn_sess, ae_sess, args.datasets_root)
        row = {**r, **b, **s}
        per_clip.append(row)
        # Compact stdout line
        ok = row["status"] == "ok"
        marker = "✓" if ok else "✗"
        if ok:
            print(f"  {marker} {r['clip_id']:<14} {scn:<22}  "
                  f"sig={row['dnsmos_sig']:.2f}  bak={row['dnsmos_bak']:.2f}  "
                  f"echo={row['aecmos_echo']:.2f}  other={row['aecmos_other']:.2f}")
        else:
            print(f"  {marker} {r['clip_id']:<14} {scn:<22}  ({row['status']})",
                  file=sys.stderr)
    print(f"\nScored {sum(1 for c in per_clip if c['status']=='ok')}/{len(per_clip)} clips")
    return 0
```

- [ ] **Step 3: Smoke-test scoring on the full corpus**

```bash
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
```

Expected:
- 30 per-clip lines with sig/bak/echo/other numbers
- `nan` for irrelevant AECMOS columns: ST_NE rows show `echo=nan`, ST_FE rows show `other=nan`
- Final: `Scored 30/30 clips`
- ~3-4 min total (DNSMOS + AECMOS inference is the dominant cost)

If `aecmos_echo` is non-NaN for ST_NE rows: SCENARIO_MAP wiring is wrong.

**No commit yet.**

---

### Task 3.4: Aggregation — per-scenario percentiles + floor/target counts

**Files:**
- Modify: `reference/run_aec_challenge.py` (add `aggregate` after main loop)

- [ ] **Step 1: Add the aggregation function**

Insert above `main()`:

```python
def aggregate(per_clip: list[dict]) -> list[dict]:
    """Per-(scenario, metric) summary: n, p10/p50/p90, floor/target counts.
    Skips NaN values (irrelevant columns per scenario)."""
    metrics = ["dnsmos_sig", "dnsmos_bak", "dnsmos_ovrl",
               "aecmos_echo", "aecmos_other", "aecmos_dt"]
    out = []
    for scn in sorted({c["scenario"] for c in per_clip}):
        rows = [c for c in per_clip if c["scenario"] == scn]
        n_ok = sum(1 for c in rows if c["status"] == "ok")
        for m in metrics:
            vals = [float(c[m]) for c in rows
                    if c["status"] == "ok"
                    and isinstance(c.get(m), (int, float))
                    and not math.isnan(float(c[m]))]
            if not vals:
                continue
            arr = np.array(vals)
            p10, p50, p90 = np.percentile(arr, [10, 50, 90])
            floor = FLOORS.get(m)
            target = TARGETS.get(m)
            n_below_floor  = sum(1 for v in vals if floor  is not None and v < floor)
            n_below_target = sum(1 for v in vals if target is not None and v < target)
            out.append({
                "scenario": scn, "metric": m, "n_clips": len(vals),
                "p10": p10, "p50": p50, "p90": p90,
                "n_below_floor": n_below_floor, "n_below_target": n_below_target,
            })
    return out
```

- [ ] **Step 2: Call aggregate at end of main**

After the scoring loop in `main()`:

```python
    summary = aggregate(per_clip)
    # Stdout table + CSV writers come in Tasks 3.5/3.6
    return 0
```

- [ ] **Step 3: Quick eyeball check**

Add a temporary `print(summary[:3])` to verify shape, run, remove the print.

**No commit yet.**

---

### Task 3.5: Output writers — per_clip.csv, summary.csv, README.txt

**Files:**
- Modify: `reference/run_aec_challenge.py` (add `write_outputs`)

- [ ] **Step 1: Add the writer**

Insert above `main()`:

```python
def write_outputs(per_clip: list[dict], summary: list[dict],
                  out_dir: Path, args: argparse.Namespace) -> None:
    # per_clip.csv
    with open(out_dir / "per_clip.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=PER_CLIP_COLS, extrasaction="ignore")
        w.writeheader()
        for r in per_clip:
            w.writerow({k: (f"{r[k]:.3f}" if isinstance(r.get(k), float)
                                              and not math.isnan(r[k])
                            else ("" if isinstance(r.get(k), float) and math.isnan(r[k])
                                  else r.get(k, "")))
                        for k in PER_CLIP_COLS})
    # summary.csv
    sum_cols = ["scenario", "metric", "n_clips",
                "p10", "p50", "p90", "n_below_floor", "n_below_target"]
    with open(out_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sum_cols)
        w.writeheader()
        for r in summary:
            w.writerow({k: (f"{r[k]:.3f}" if isinstance(r[k], float) else r[k])
                        for k in sum_cols})
    # README.txt — provenance
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip() or "unknown"
    with open(out_dir / "README.txt", "w") as f:
        f.write(f"Invocation: {' '.join(shlex.quote(a) for a in sys.argv)}\n")
        f.write(f"git rev:    {rev}\n")
        f.write(f"bench:      {args.bench}\n")
        f.write(f"agc:        {args.agc}\n")
        f.write(f"bench_flags: {args.bench_flags}\n")
        f.write(f"manifest:   {args.manifest}\n")
        f.write(f"n_clips:    {len(per_clip)}\n")
```

- [ ] **Step 2: Call from main**

After `summary = aggregate(per_clip)`:

```python
    write_outputs(per_clip, summary, args.out_dir, args)
    print(f"\nWrote {args.out_dir}/per_clip.csv, summary.csv, README.txt")
    if not args.keep_enh_wavs:
        import shutil
        shutil.rmtree(args.out_dir / "enh")
```

- [ ] **Step 3: Smoke-test outputs**

```bash
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
ls -la /tmp/run_smoke/
head -5 /tmp/run_smoke/per_clip.csv
head -5 /tmp/run_smoke/summary.csv
cat /tmp/run_smoke/README.txt
```

Expected:
- 30+ rows in per_clip.csv (header + 30 clips)
- ~15 rows in summary.csv (3 scenarios × ~5-6 applicable metrics)
- README.txt with invocation, git rev, args

**No commit yet.**

---

### Task 3.6: Stdout summary tables + CORPUS VERDICT

**Files:**
- Modify: `reference/run_aec_challenge.py` (add `print_summary`)

- [ ] **Step 1: Add the summary printer**

Insert above `main()`:

```python
def _colour(v: float, floor: float | None, target: float | None) -> str:
    if floor is None or target is None or math.isnan(v):
        return f"{v:6.2f}"
    if v < floor:  return f"\033[31m{v:6.2f}\033[0m"
    if v < target: return f"\033[33m{v:6.2f}\033[0m"
    return f"\033[32m{v:6.2f}\033[0m"


def print_summary(per_clip: list[dict], summary: list[dict]) -> bool:
    """Print per-scenario tables + CORPUS VERDICT. Returns True if PASS."""
    corpus_pass = True
    for scn in sorted({s["scenario"] for s in summary}):
        scn_rows = [r for r in per_clip if r["scenario"] == scn]
        n_total = len(scn_rows)
        n_ok = sum(1 for r in scn_rows if r["status"] == "ok")
        n_failed = n_total - n_ok
        print(f"\n═══ {scn} ═══   ({n_total} clips, {n_ok} ok, {n_failed} failed)")
        print(f"  {'metric':<14} {'p10':>6} {'p50':>6} {'p90':>6}  "
              f"{'floor':>6} {'target':>7}  verdict")
        for s in [s for s in summary if s["scenario"] == scn]:
            m = s["metric"]
            floor, target = FLOORS.get(m), TARGETS.get(m)
            floor_s  = f"{floor:>6.2f}"  if floor  is not None else "    — "
            target_s = f"{target:>7.2f}" if target is not None else "    —  "
            verdict = "✓" if (floor is not None and s["p50"] >= floor) else \
                      f"{s['n_below_floor']}/{s['n_clips']} below floor" \
                          if floor is not None else "(informational)"
            print(f"  {m:<14} {_colour(s['p10'], floor, target)} "
                  f"{_colour(s['p50'], floor, target)} "
                  f"{_colour(s['p90'], floor, target)}  "
                  f"{floor_s} {target_s}  {verdict}")
            if floor is not None and s["p50"] < floor:
                corpus_pass = False
        if n_failed >= 3:
            print(f"  \033[31mWARNING: {n_failed} clips failed in {scn}\033[0m")
    print("\n" + ("=" * 50))
    print(f"CORPUS VERDICT: {'\033[32mPASS\033[0m' if corpus_pass else '\033[31mBLOCK\033[0m'}")
    print("=" * 50)
    return corpus_pass
```

- [ ] **Step 2: Wire into main + set exit code**

Update end of `main()`:

```python
    write_outputs(per_clip, summary, args.out_dir, args)
    print(f"\nWrote {args.out_dir}/per_clip.csv, summary.csv, README.txt")
    if not args.keep_enh_wavs:
        for r in per_clip:
            enh = args.out_dir / f"{r['clip_id']}_enh.wav"
            if enh.exists(): enh.unlink()
    pass_ok = print_summary(per_clip, summary)
    # Exit 0 on PASS, 1 on BLOCK, 2 on cache/fetch issues (handled earlier).
    return 0 if pass_ok else 1
```

- [ ] **Step 3: Smoke-test the output**

```bash
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
echo "exit: $?"
```

Expected:
- 3 ANSI-coloured tables (doubletalk / nearend_singletalk / farend_singletalk)
- Final CORPUS VERDICT line
- Exit 0 or 1 depending on whether the chain clears all floors

**No commit yet — error handling + final polish in Task 3.7.**

---

### Task 3.7: Error-handling matrix per spec

**Files:**
- Modify: `reference/run_aec_challenge.py` (add scenario-failure exit check)

- [ ] **Step 1: Add the per-scenario failure check**

After `print_summary(per_clip, summary)` in `main()`:

```python
    # Per error-handling matrix: exit 1 if any scenario has all clips failed.
    failed_scenarios = []
    for scn in {r["scenario"] for r in per_clip}:
        scn_rows = [r for r in per_clip if r["scenario"] == scn]
        if scn_rows and all(r["status"] != "ok" for r in scn_rows):
            failed_scenarios.append(scn)
    if failed_scenarios:
        print(f"\nERROR: all clips failed in scenario(s): {failed_scenarios}",
              file=sys.stderr)
        return 1
    return 0 if pass_ok else 1
```

- [ ] **Step 2: Verify error matrix behaves**

Simulate a scenario-wide failure by passing a bad bench flag:

```bash
python3 reference/run_aec_challenge.py \
    --bench ./build/ecnr_bench \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --bench-flags "--this-flag-does-not-exist" \
    --out-dir /tmp/run_smoke_fail
echo "exit: $?"
```

Expected:
- All 30 clips fail with `bench_failed`
- "WARNING: 10 clips failed in <scenario>" printed thrice
- "ERROR: all clips failed in scenario(s): [...]"
- Exit 1

**No commit yet.**

---

### Task 3.8: Smoke-test full pipeline + commit runner

**Files:** (no new — final verify of run_aec_challenge.py)

- [ ] **Step 1: Final clean run**

```bash
rm -rf /tmp/run_smoke
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/run_smoke
echo "exit: $?"
```

Expected: clean run, 30 clips scored, 3 scenario tables + CORPUS VERDICT, CSVs + README.txt in place, exit 0 or 1.

- [ ] **Step 2: Spot-check one enhanced WAV**

```bash
ls /tmp/run_smoke/*_enh.wav | head -3
afplay $(ls /tmp/run_smoke/*_enh.wav | head -1)
```

Expected: audible chain output (not silence, not glitchy). If silence: the bench failed silently — check status column in per_clip.csv.

- [ ] **Step 3: Commit**

```bash
git add reference/run_aec_challenge.py
git commit -m "$(cat <<'EOF'
feat(eval): AEC-Challenge corpus runner — scenario-aware AECMOS + DNSMOS

reference/run_aec_challenge.py iterates the MANIFEST.tsv-locked 30-clip
subset, runs ecnr_bench per clip, scores each output with DNSMOS P.835
plus AECMOS under the talk_type matching the clip's scenario
(doubletalk → dt, near-end single-talk → st_ne, far-end single-talk →
st_fe). Aggregates per-scenario p10/p50/p90, counts how many clips
fall below the ADR-0012 floors/targets, prints a coloured per-scenario
table and a final CORPUS VERDICT line.

Outputs to <out-dir>: per_clip.csv (one row per clip + status),
summary.csv (long-format per scenario × metric), README.txt
(invocation + git rev), enh/*.wav (per-clip chain output, --no-keep-enh-wavs to skip).

Exit code: 0 if every scenario's p50 clears every applicable floor,
1 on BLOCK or per-scenario total failure, 2 on cache miss / SHA drift.

Per design spec
docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 4 — First diagnostic run + grade doc

### Task 4.1: Run against the full corpus, two configs (AGC off / AGC on)

**Files:** (no code — produces /tmp artifacts for Task 4.2)

- [ ] **Step 1: AGC-off run (current production default)**

```bash
rm -rf /tmp/aec_grade_agcoff
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/aec_grade_agcoff 2>&1 | tee /tmp/aec_grade_agcoff/run.log
```

Expected: 30 clips scored, 3 scenario tables + CORPUS VERDICT.

- [ ] **Step 2: AGC-on run**

```bash
rm -rf /tmp/aec_grade_agcon
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --agc \
    --out-dir /tmp/aec_grade_agcon 2>&1 | tee /tmp/aec_grade_agcon/run.log
```

- [ ] **Step 3: Capture both summary tables for the grade doc**

```bash
cat /tmp/aec_grade_agcoff/summary.csv
cat /tmp/aec_grade_agcon/summary.csv
```

Keep these visible — they're the source data for Task 4.2.

---

### Task 4.2: Write the grade doc

**Files:**
- Create: `docs/phase-1-acceptance-grade-aec-challenge.md`

- [ ] **Step 1: Draft the doc**

Structure (parallels `docs/phase-1-acceptance-grade.md`):

```markdown
# Phase 1 Acceptance Grade — AEC-Challenge First Run (YYYY-MM-DD)

> First diagnostic run of the current chain against the
> Microsoft AEC-Challenge 30-clip stratified subset.
> Per design spec `docs/superpowers/specs/2026-05-31-aec-challenge-integration-design.md`.

## TL;DR

<one-paragraph summary of the CORPUS VERDICT for both AGC off and AGC on,
noting which floors are cleared / blocked per scenario.>

## Setup

| | |
|---|---|
| Chain version  | `main` at commit `<git rev from README.txt>` |
| Corpus         | AEC-Challenge real-recordings, 30 clips (10 each: doubletalk, nearend_singletalk, farend_singletalk) |
| Manifest       | `datasets/aec_challenge/MANIFEST.tsv` |
| Scoring        | DNSMOS P.835 + AECMOS (talk_type per scenario) |
| Gate           | ADR-0012 §2 floors/targets (unchanged — first run is diagnostic only) |

## Measured numbers — AGC OFF (production default)

### doubletalk
<paste-from-summary.csv as a table>

### nearend_singletalk
<table>

### farend_singletalk
<table>

CORPUS VERDICT (AGC off): <PASS or BLOCK + which scenarios + which metrics>

## Measured numbers — AGC ON
<same shape>

## Findings

### 1. <observation that pops out of the numbers>
<2-3 sentences>

### 2. <next observation>
...

## Implications

<sentences about whether ADR-0012 floors are defensible at these numbers,
whether the chain is closer/further from passing than the synthetic-fixture
grade suggested, whether a sweep is warranted, etc. Do not modify ADR-0012;
just observations.>

## Reproduction

```sh
# Once: fetch the 30-clip subset (idempotent — skips cache hits)
python3 reference/fetch_aec_challenge.py

# Run the chain against the corpus
mkdir -p /tmp/aec_grade
python3 reference/run_aec_challenge.py \
    --dnsmos-model models/dnsmos_p835.onnx \
    --aecmos-model models/aecmos.onnx \
    --out-dir /tmp/aec_grade
```

## References
- [ADR-0012](adr/0012-phase-1-acceptance-bar.md) — the gate this corpus feeds into
- [Design spec](superpowers/specs/2026-05-31-aec-challenge-integration-design.md)
- [Previous synthetic-fixture grade](phase-1-acceptance-grade.md) — for comparison
- [Microsoft AEC-Challenge](https://github.com/microsoft/AEC-Challenge)
```

- [ ] **Step 2: Fill in the numbers**

For each scenario, take the `summary.csv` rows and format them as a markdown table:

```
| metric         | p10  | p50  | p90  | floor | target | n below floor |
|---|---:|---:|---:|---:|---:|---:|
| dnsmos_sig     | 2.41 | 2.78 | 3.12 | 3.0   | 3.5    | 7/10          |
| ...
```

For each metric where p50 < floor: bold the verdict cell. For each where p50 < target but ≥ floor: italicize.

- [ ] **Step 3: Write the Findings + Implications sections**

Look at the actual numbers and write 2-4 observations. Likely directions (don't preempt — let the data lead):
- Which scenario is worst hit by which metric
- How AGC-on changes things (compare the two tables)
- Whether the per-scenario verdict shape matches or contradicts the previous synthetic-fixture grade
- Whether any metric clears all three scenarios (rare = corpus-wide healthy axis)

Keep observations factual; defer any ADR-0012 floor recalibration to a follow-up decision (per spec non-goal).

- [ ] **Step 4: Commit**

```bash
git add docs/phase-1-acceptance-grade-aec-challenge.md
git commit -m "$(cat <<'EOF'
docs(grade): AEC-Challenge first-run diagnostic — chain vs 30-clip subset

First diagnostic grade of the current chain against the Microsoft
AEC-Challenge real-recordings 30-clip subset (10 each: doubletalk /
nearend_singletalk / farend_singletalk), AGC off and AGC on.

CORPUS VERDICT: <PASS or BLOCK>
<one-line summary of dominant failure axis if BLOCK, else of the
quality margin if PASS>

ADR-0012 floors/targets unchanged — first run is diagnostic only,
per spec non-goal. Recalibration deferred until we've seen the
spread across multiple chain configs (next: NS-blend sweep
against this corpus, deferred).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist (do at end)

After all phases complete, run through this once:

- [ ] All 8 decisions from the spec's "Decisions made during brainstorming" table are reflected in code/docs
- [ ] `score_aecmos_dt` shim is present in `score_mos.py` (back-compat preserved)
- [ ] `sweep_ns_blend.py` still works (smoke-run it once to confirm)
- [ ] `MANIFEST.tsv` is committed, WAVs are gitignored
- [ ] `.gitignore` allows `datasets/**/MANIFEST.tsv` through `*.wav`
- [ ] `per_clip.csv` and `summary.csv` have the columns the spec specifies
- [ ] Exit codes: 0 = PASS, 1 = BLOCK, 2 = cache/fetch issue
- [ ] Grade doc is committed; ADR-0012 was NOT modified (per spec non-goal)

---

## Open follow-ups (NOT in this plan — for future PRs)

1. NS-blend sweep against the AEC-Challenge corpus (Approach C from brainstorm — extend `sweep_ns_blend.py` with `--manifest`).
2. ADR-0012 floor/target recalibration based on multiple-config corpus data.
3. CI wiring (manual-trigger workflow that runs `fetch` + `run` against the corpus on `src/pipeline/` PRs).
4. Synthetic test set integration (adds true-ERLE + SI-SDR for a separate corpus, complementary to the real one).
