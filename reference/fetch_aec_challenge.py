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
    # doubletalk (10)
    ("doubletalk",         "dt_01", "-2jLGNCgf0WDpKMY2iup7g"),
    ("doubletalk",         "dt_02", "-3Jxai1udE6V32_c-aUJFA"),
    ("doubletalk",         "dt_03", "-3kWiJ8CXU6ZtIZn1yf8bg"),
    ("doubletalk",         "dt_04", "-6J2UYFUOkCkA0FbNaRyOQ"),
    ("doubletalk",         "dt_05", "-6ZM9_DKN0WHCo5oYppZhQ"),
    ("doubletalk",         "dt_06", "-9OpPnj1J02C6RkA_o09dQ"),
    ("doubletalk",         "dt_07", "-9Qt1FJHC0afHFxhsdVzxw"),
    ("doubletalk",         "dt_08", "-AQgRZRHXkKqI7jpf32Fyg"),
    ("doubletalk",         "dt_09", "-BcqKIzPkEmJloA1Ty2ObA"),
    ("doubletalk",         "dt_10", "-CzGajBhQkeK7COgs6bscw"),

    # farend_singletalk (10)
    ("farend_singletalk",  "fe_01", "-0AcvGNEdEK-DQGxWmtq2Q"),
    ("farend_singletalk",  "fe_02", "-2jLGNCgf0WDpKMY2iup7g"),
    ("farend_singletalk",  "fe_03", "-3Jxai1udE6V32_c-aUJFA"),
    ("farend_singletalk",  "fe_04", "-3kWiJ8CXU6ZtIZn1yf8bg"),
    ("farend_singletalk",  "fe_05", "-4ZzhygVJ0i2f7wMpKYIqg"),
    ("farend_singletalk",  "fe_06", "-6J2UYFUOkCkA0FbNaRyOQ"),
    ("farend_singletalk",  "fe_07", "-6ZM9_DKN0WHCo5oYppZhQ"),
    ("farend_singletalk",  "fe_08", "-9OpPnj1J02C6RkA_o09dQ"),
    ("farend_singletalk",  "fe_09", "-9Qt1FJHC0afHFxhsdVzxw"),
    ("farend_singletalk",  "fe_10", "-AQgRZRHXkKqI7jpf32Fyg"),

    # nearend_singletalk (10) — lpb is synthesized
    ("nearend_singletalk", "ne_01", "-0AcvGNEdEK-DQGxWmtq2Q"),
    ("nearend_singletalk", "ne_02", "-2jLGNCgf0WDpKMY2iup7g"),
    ("nearend_singletalk", "ne_03", "-34oOakz8EuGA0OD8lpAOw"),
    ("nearend_singletalk", "ne_04", "-3Jxai1udE6V32_c-aUJFA"),
    ("nearend_singletalk", "ne_05", "-4ZzhygVJ0i2f7wMpKYIqg"),
    ("nearend_singletalk", "ne_06", "-6J2UYFUOkCkA0FbNaRyOQ"),
    ("nearend_singletalk", "ne_07", "-6ZM9_DKN0WHCo5oYppZhQ"),
    ("nearend_singletalk", "ne_08", "-6c7f0rQV0iN6BWQE6sULw"),
    ("nearend_singletalk", "ne_09", "-9OpPnj1J02C6RkA_o09dQ"),
    ("nearend_singletalk", "ne_10", "-9Qt1FJHC0afHFxhsdVzxw"),
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
