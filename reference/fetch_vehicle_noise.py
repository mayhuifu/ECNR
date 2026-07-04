#!/usr/bin/env python3
"""Fetch the pinned real-recording assets for the GB/T 45314 pre-compliance
conditions (research memo 2026-07-04; spec §2 approach B).

Two fully scriptable, license-clean sources (the ETSI docbox noise databases
are cookie-gated and deliberately NOT fetched here):

* DEMAND `TCAR` (in-car driving noise, 16 kHz 16-channel, 300 s) — Zenodo,
  CC BY 4.0, DOI 10.5281/zenodo.1227121. We keep only channel 1.
* ITU-T P.501-lineage fullband speech (48 kHz mono, ~8 s each) mirrored in
  microsoft/P.808 `p835_reference_conditions/3gpp_p501_FB/`. ITU grants a
  free license for conformance-testing use (license text fetched alongside).
  GB/T 45314-2025 normatively references ITU-T P.501 — using P.501-lineage
  speech is the closest scriptable match to the standard's stimuli.

Modes (mirrors reference/fetch_aec_challenge.py):
  default      validate everything against MANIFEST.tsv, fetch what's missing
  --bootstrap  download, compute SHA256s, (re)write MANIFEST.tsv

The 130 MB TCAR zip is cached under datasets/vehicle_noise/.cache/ so
re-validation never re-downloads it. All WAVs are gitignored; only
MANIFEST.tsv is committed.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "datasets" / "vehicle_noise"
CACHE = DEST / ".cache"
MANIFEST = DEST / "MANIFEST.tsv"

TCAR_ZIP_URL = "https://zenodo.org/records/1227121/files/TCAR_16k.zip"
TCAR_ZIP_NAME = "TCAR_16k.zip"
TCAR_MEMBER_SUFFIX = "ch01.wav"  # first array channel is enough for a mono NS/AEC chain
TCAR_OUT = "tcar_ch01_16k.wav"

P501_BASE = ("https://raw.githubusercontent.com/microsoft/P.808/master/"
             "p835_reference_conditions/3gpp_p501_FB/")
# Two female + two male talkers, distinct sentence sets: enough for
# far-end / near-end double-talk pairs without reusing material.
P501_FILES = ["i01_f1.wav", "i01_m1.wav", "i02_f2.wav", "i02_m2.wav"]
P501_LICENSE = "itu_license_text_from_P501.txt"


def sha256_of(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: pathlib.Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")
    print(f"  fetching {url}")
    if shutil.which("curl"):
        # curl for resilience on the 130 MB Zenodo zip: -C - resumes a
        # partial .part across retries (urllib died mid-stream with
        # SSL UNEXPECTED_EOF on first bootstrap, 2026-07-04).
        subprocess.run(
            ["curl", "-fL", "--retry", "5", "--retry-all-errors",
             "-C", "-", "-o", str(part), url],
            check=True)
    else:
        req = urllib.request.Request(url, headers={"User-Agent": "ecnr-fetch/1.0"})
        with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
    part.replace(dest)


def ensure_tcar_wav() -> pathlib.Path:
    """Download the TCAR zip (cached) and extract channel 1."""
    zip_path = CACHE / TCAR_ZIP_NAME
    if not zip_path.exists():
        download(TCAR_ZIP_URL, zip_path)
    out = DEST / TCAR_OUT
    if not out.exists():
        with zipfile.ZipFile(zip_path) as z:
            members = [m for m in z.namelist() if m.endswith(TCAR_MEMBER_SUFFIX)]
            if not members:
                sys.exit(f"no member ending in {TCAR_MEMBER_SUFFIX} inside {zip_path}")
            data = z.read(members[0])
        tmp = out.with_suffix(".wav.part")
        tmp.write_bytes(data)
        tmp.replace(out)
        print(f"  extracted {members[0]} -> {out.name} ({len(data)} bytes)")
    return out


def ensure_p501() -> list[pathlib.Path]:
    outs = []
    for name in P501_FILES + [P501_LICENSE]:
        out = DEST / "p501" / name
        if not out.exists():
            download(P501_BASE + name, out)
        outs.append(out)
    return [p for p in outs if p.suffix == ".wav"]


def all_assets() -> list[pathlib.Path]:
    return [DEST / TCAR_OUT] + [DEST / "p501" / n for n in P501_FILES]


def bootstrap() -> int:
    ensure_tcar_wav()
    ensure_p501()
    rows = ["filename\tsha256\tsource"]
    zip_path = CACHE / TCAR_ZIP_NAME
    rows.append(f".cache/{TCAR_ZIP_NAME}\t{sha256_of(zip_path)}\t{TCAR_ZIP_URL}")
    for p in all_assets():
        rel = p.relative_to(DEST).as_posix()
        src = TCAR_ZIP_URL if p.name == TCAR_OUT else P501_BASE + p.name
        rows.append(f"{rel}\t{sha256_of(p)}\t{src}")
    MANIFEST.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"wrote {MANIFEST} ({len(rows) - 1} pinned files)")
    return 0


def validate() -> int:
    if not MANIFEST.exists():
        sys.exit(f"{MANIFEST} missing — run with --bootstrap first (network needed)")
    failures = 0
    for line in MANIFEST.read_text(encoding="utf-8").splitlines()[1:]:
        rel, want_sha, src = line.split("\t")
        path = DEST / rel
        if not path.exists():
            if rel.startswith(".cache/"):
                # The zip cache is only needed to (re)extract; skip if the
                # extracted wav is present and valid.
                if (DEST / TCAR_OUT).exists():
                    continue
                download(TCAR_ZIP_URL, path)
            elif rel == TCAR_OUT:
                ensure_tcar_wav()
            else:
                download(src, path)
        got = sha256_of(path)
        if got != want_sha:
            print(f"  SHA MISMATCH {rel}\n    want {want_sha}\n    got  {got}",
                  file=sys.stderr)
            failures += 1
        else:
            print(f"  ok  {rel}")
    if failures:
        sys.exit(f"{failures} file(s) failed validation")
    print("vehicle_noise assets valid")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bootstrap", action="store_true",
                    help="download + compute SHAs + write MANIFEST.tsv")
    args = ap.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    return bootstrap() if args.bootstrap else validate()


if __name__ == "__main__":
    raise SystemExit(main())
