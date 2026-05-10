# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository nature

This is a **research / documentation-only repository** — there is no source code, no build system, no tests, and no package manifest. It collects long-form technical research on acoustic echo cancellation (AEC) and noise suppression (NS) for cellular voice (VoLTE / VoNR / 5G MTSI) terminals.

Do not invent commands, scripts, or tooling. There is nothing to build, lint, or run.

## Contents

All material lives under `docs/`:

- `docs/deep-research-report.md` — Chinese-language research report: *面向 VoLTE 与 VoNR 终端的 AEC 与 NS 算法研究报告*. Covers the classical DSP vs. neural/hybrid algorithm landscape, EVS/AMR/AMR-WB constraints from 3GPP TS 26.114, open-source stacks (WebRTC AEC3, SpeexDSP, RNNoise, DTLN-AEC, NKF-AEC, DeepFilterNet), commercial IP (Cadence Tensilica HiFi, CEVA ClearVox, NXP VoiceSeeker), and Cortex-A55 deployment strategy.
- `docs/Cellular Audio Processing Solutions Deep Dive.md` — English-language counterpart: *Advanced Audio Algorithms for AEC and NR in 5G VoLTE and VoNR Applications*. Same problem space, more focused on vendor DSP architectures (Tensilica HiFi iQ, CEVA SensPro2, NXP SAF9100) and hybrid DSP-DNN topologies.
- `docs/5G_VoNR_Audio_Architecture.pdf` — primary-source PDF the markdown reports draw from.

The two markdown files are largely parallel takes (one CN, one EN) on the same body of source material; expect overlapping claims and citations. They are not a translation pair — content and emphasis differ.

## Working in this repo

- Treat the `.md` files as the canonical artifacts. Edit them directly when the user asks to revise, restructure, or extend the research.
- Both reports use inline `citeturnNsearchM` / `urlFOOturnN` markers — these are reference-tracker tokens from the original research tool, not standard markdown. Preserve them verbatim unless the user asks to clean them up.
- The CN report mixes Simplified Chinese prose with English technical terms (AEC, NLMS, RLS, EVS, JBM, etc.) — keep that bilingual style when editing it; do not translate technical terms into Chinese.
- The PDF is large (~20 MB) — read specific page ranges with the `pages` parameter rather than the whole file.
