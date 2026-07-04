# A55 optimization loop — iteration log

Protocol per `docs/superpowers/specs/2026-07-04-a55-perf-loop-and-gbt45314-corpus-design.md` §3:
one move per iteration → build → **AEC-Challenge gate must stay PASS** (+ GB/T 45314 verdict tracked, 42 unit tests green) → measure → keep or revert with the numbers written down. Quality floors are the arbiter, never taste.

**Measurement fixture:** 30-clip AEC-Challenge corpus via `run_aec_challenge.py` (`cpu_ms_per_frame` = chain-only time, beamformer bypassed, AGC on); single-clip probes on the first doubletalk clip (12.26 s, 1226 frames); RSS via `/usr/bin/time -l`. Host = Apple Silicon macOS, Release, webrtc-apm `--buildtype=release` with **NEON off** (upstream disables it on darwin — host numbers underestimate APM on-target).

## Baseline (2026-07-04, main @ da28622)

| Metric | Value |
|---|---|
| AEC-Challenge corpus | **PASS** (DT sig 3.02 / ovrl 2.71 / echo 4.46 / dt 3.28 p50s) |
| cpu_ms_per_frame (corpus mean / p50 / max) | 0.201 / 0.208 / 0.227 |
| Host RTF | 0.020 |
| Per-stage split (probe clip, AGC on) | AEC3 0.122 ms (55 %) · NS 0.086 ms (39 %) · AGC2 0.013 ms (6 %) · BF/render ≈ 0 |
| Peak RSS / peak footprint | 14.5 MB / 4.4 MB |
| Host binary (Release / stripped) | 4.75 / 4.57 MB |
| aarch64 binary (Release+strip, int8) | 4.25 MB · qemu RTF 0.81 (2026-05 spike, cortexa57 tune) |

A55 projection at 10–15× host derate: **~2–3 ms per 10 ms frame ≈ 20–30 % of one core** — the budget Phase-3 RES has to fit beside.

## Iterations

| # | Move | Result | Verdict |
|---|---|---|---|
| M0 | Per-stage `ChainStats` timers + bench columns (`cpu_bf/aec/ns/agc/render`) | Split above; overhead unmeasurable (5 clock reads/frame) | **KEEP** (228888e) |
| M3 | Merge AGC2 into the AEC3 APM instance | Closed by M0's measurement: 2nd APM = 6 % CPU — not worth breaking ADR-0001 post-NS ordering | **CLOSED, keep split** |
| M1 | Drop `Frame::ch{}` value-init (~15 KB memset / frame) | Host-neutral (p50 0.208→0.209, noise); contract already declared tail scratch; motivated by A55 32 KB L1D | **KEEP** (1c6d220) |
| M7 | AEC3 filter-length knob (`--aec-filter-blocks`, 9–20; default 13) | `cpu_aec` **flat** across 9–13 → host AEC3 budget sits in matched-filter delay estimator + FFTs, not main filters. ERLE on probe clip mildly better at 10–12 (7.2–7.5 vs 7.09). <9 aborts in `RenderDelayBuffer` sizing | **KEEP as ADR-0011 tuning knob, no perf claim; default unchanged** (2440dd2) |
| M4 | Cross-build `ecnr_rnnoise` with `-mcpu=cortex-a55` (DotProd SDOT for the int8 GEMV; a57 stand-in tune left it dead) | Host-invisible by construction; qemu/on-target A/B pending (cross-build in flight) | **KEEP, scoped to hot lib** (2440dd2) |
| M2 | Speex resampler quality 5→3 on the NS path | `cpu_ns` unchanged (RNNoise inference dominates; resamplers are noise) | **REVERT, q5 kept** |

## Standing conclusions

1. **Host-side config tuning is exhausted at ~0.2 ms/frame.** The two dominant blocks (AEC3 fixed machinery, RNNoise int8 inference) don't respond to the available knobs on the host build.
2. **The open CPU levers are target-side:** DotProd int8 GEMV (M4, pending qemu numbers) and the NEON-enabled APM build the cross-compile already uses. Re-run the per-stage split on-target when U300 hardware lands — the host 55/39/6 mix will shift.
3. **Memory is not the constraint today:** 14.5 MB RSS / 4.25 MB binary against an automotive-grade U300. The second APM instance is the only known structural saving (~its heap) and is deliberately kept for ADR-0001 ordering.
4. Next levers worth a look when on-target numbers exist: APM submodule audit on instance #1 (TransientSuppressor etc. if enabled by default), `-Os` on non-hot libs for code size, and the Phase-3 RES budget planning this loop exists to serve.
