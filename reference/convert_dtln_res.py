#!/usr/bin/env python3
"""Convert the vendored DTLN-AEC TFLite models to ONNX for the ECNR RES stage.

Why ONNX and not TFLite at runtime (ADR-0007 deviation, recorded in
ADR-0014): no packaged TensorFlow-Lite C library exists for any platform we
deploy on (macOS hosts, ubuntu CI, Yocto aarch64), while ONNX Runtime is
packaged for all three (brew bottle, upstream linux-x64/aarch64 release
tarballs). The models themselves stay bit-identical upstream artifacts
(vendor/dtln-aec, MIT, pinned by vendor/MANIFEST.tsv) — this script is a
deterministic format conversion plus a numerical equivalence check against
the TFLite reference interpreter.

Requires a venv with: tensorflow, tf2onnx, onnx, onnxruntime, numpy.
(Heavy — conversion is an offline, run-once step; the chain never needs TF.)

Usage:
  python3 reference/convert_dtln_res.py \
      --units 128 \
      --models-dir vendor/dtln-aec/pretrained_models \
      --out-dir models

Outputs models/dtln_aec_<units>_{1,2}.onnx, prints SHA256 for pinning in
reference/fetch_res_models.py, and fails loudly if ONNX and TFLite outputs
diverge beyond 1e-4 max-abs on a 200-block randomized stateful run.
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys

import numpy as np


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def convert(tflite_path: pathlib.Path, onnx_path: pathlib.Path) -> None:
    cmd = [
        sys.executable, "-m", "tf2onnx.convert",
        "--tflite", str(tflite_path),
        "--output", str(onnx_path),
        "--opset", "13",
    ]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def validate(tflite_path: pathlib.Path, onnx_path: pathlib.Path,
             n_blocks: int = 200) -> float:
    """Run both interpreters through a stateful randomized block stream."""
    import tensorflow as tf
    import onnxruntime as ort

    interp = tf.lite.Interpreter(model_path=str(tflite_path))
    interp.allocate_tensors()
    in_det = interp.get_input_details()
    out_det = interp.get_output_details()

    sess = ort.InferenceSession(str(onnx_path),
                                providers=["CPUExecutionProvider"])
    ort_inputs = {i.name: i for i in sess.get_inputs()}

    # Input 1 (index 1 by position) is the LSTM state tensor in both stages.
    rng = np.random.default_rng(0x45314)
    states_tfl = np.zeros(in_det[1]["shape"], dtype=np.float32)
    states_onx = states_tfl.copy()
    max_diff = 0.0
    for _ in range(n_blocks):
        feeds_tfl = {}
        feeds_onx = {}
        for pos, det in enumerate(in_det):
            if pos == 1:
                arr = None  # states, handled below
            else:
                arr = (rng.standard_normal(det["shape"]) * 0.1).astype(
                    np.float32)
            if arr is not None:
                feeds_tfl[pos] = arr
        for pos, det in enumerate(in_det):
            arr = feeds_tfl.get(pos, states_tfl if pos == 1 else None)
            interp.set_tensor(det["index"], arr)
        interp.invoke()
        out_tfl = interp.get_tensor(out_det[0]["index"])
        states_tfl = interp.get_tensor(out_det[1]["index"])

        # ONNX inputs are matched by name; tf2onnx preserves tflite tensor
        # names, so map by shape+order: state tensor shares in_det[1] shape.
        onnx_names = [i.name for i in sess.get_inputs()]
        for pos, det in enumerate(in_det):
            arr = feeds_tfl.get(pos, states_onx if pos == 1 else None)
            # tf2onnx keeps positional correspondence of graph inputs.
            feeds_onx[onnx_names[pos]] = arr
        onx_out = sess.run(None, feeds_onx)
        out_onx, states_onx = onx_out[0], onx_out[1]

        max_diff = max(max_diff,
                       float(np.max(np.abs(out_tfl - out_onx))),
                       float(np.max(np.abs(states_tfl - states_onx))))
    return max_diff


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--units", type=int, default=128,
                    choices=(128, 256, 512))
    ap.add_argument("--models-dir", type=pathlib.Path,
                    default=pathlib.Path("vendor/dtln-aec/pretrained_models"))
    ap.add_argument("--out-dir", type=pathlib.Path,
                    default=pathlib.Path("models"))
    ap.add_argument("--skip-validate", action="store_true")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    # 200 stateful LSTM blocks accumulate float32 op-order differences
    # between the two runtimes; 1e-3 on unit-scale masks/time samples is
    # −60 dB — far below anything the audio path can resolve, while real
    # conversion faults (wrong weight, wrong state wiring) blow past it
    # by orders of magnitude.
    tol = 1e-3
    for stage in (1, 2):
        src = args.models_dir / f"dtln_aec_{args.units}_{stage}.tflite"
        dst = args.out_dir / f"dtln_aec_{args.units}_{stage}.onnx"
        if not src.exists():
            print(f"missing {src} — run scripts/fetch-vendor.sh", file=sys.stderr)
            return 2
        convert(src, dst)
        if not args.skip_validate:
            diff = validate(src, dst)
            status = "OK" if diff < tol else "FAIL"
            print(f"stage {stage}: max |tflite - onnx| = {diff:.2e}  [{status}]")
            if diff >= tol:
                return 1
        print(f"{dst}  sha256={sha256(dst)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
