#!/usr/bin/env python3
"""Fetch the DTLN-AEC ONNX models for the Phase-3 RES hybrid stage.

The models are deterministic conversions of the vendored MIT-licensed
DTLN-AEC pretrained TFLite artifacts (vendor/dtln-aec, pinned by
vendor/MANIFEST.tsv) — see reference/convert_dtln_res.py for the
conversion + numerical-equivalence pipeline. They are distributed as
GitHub release assets (repo policy: model binaries are never committed;
they are SHA256-pinned and fetched on demand, same as
datasets/aec_challenge and the DNSMOS/AECMOS models).

Usage:
  python3 reference/fetch_res_models.py [--units 256] [--out-dir models]

Exit codes: 0 = all present + verified, 1 = download/verify failure.
"""

import argparse
import hashlib
import pathlib
import sys
import urllib.request

RELEASE_URL = "https://github.com/mayhuifu/ECNR/releases/download/v0.4.2"

# sha256 pins for the converted models (tf2onnx 1.17.0, opset 13; conversion
# validated ≤ 1.3e-4 vs the TFLite reference over a 200-block stateful run).
MODELS = {
    "dtln_aec_128_1.onnx":
        "8234b843827701d64e5f84541b86aa658942b43f04d432883b732c3812d4c3e7",
    "dtln_aec_128_2.onnx":
        "3fd992cb89034cbe50fe4be7f0bb914129caba223862101fec5824611ba00b9b",
    "dtln_aec_256_1.onnx":
        "602fd28d6f8a6566dc6daf5f453eeeefc8acf46cadf4ef03cd13e0f2a609f29d",
    "dtln_aec_256_2.onnx":
        "36ca8c452ae751a7c87bdf5870f57bda8a4f0bf849491fe272bbe996e45ae7cf",
}


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--units", type=int, default=0,
                    help="fetch only this variant (128 or 256); 0 = all")
    ap.add_argument("--out-dir", type=pathlib.Path,
                    default=pathlib.Path("models"))
    args = ap.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    wanted = {n: s for n, s in MODELS.items()
              if args.units == 0 or f"_{args.units}_" in n}
    ok = True
    for name, want in wanted.items():
        dst = args.out_dir / name
        if dst.exists() and sha256(dst) == want:
            print(f"  ok       {name}")
            continue
        url = f"{RELEASE_URL}/{name}"
        tmp = dst.with_suffix(".part")
        print(f"  fetching {name}")
        try:
            urllib.request.urlretrieve(url, tmp)
        except OSError as e:
            print(f"  FAILED   {name}: {e}", file=sys.stderr)
            ok = False
            continue
        got = sha256(tmp)
        if got != want:
            print(f"  FAILED   {name}: sha256 mismatch\n"
                  f"           want {want}\n           got  {got}",
                  file=sys.stderr)
            tmp.unlink(missing_ok=True)
            ok = False
            continue
        tmp.replace(dst)
        print(f"  ok       {name}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
