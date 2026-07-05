# Public dataset survey for ECNR validation — 2026-07-05

URL-verified survey (HTTP status + size checked at fetch time) of publicly
downloadable corpora for automotive AEC + NS testing. Produced for the
extended-validation pass in docs/validation-extended-2026-07-05.md.

All URLs below were fetched 2026-07-05 (HTTP status + Content-Length from `curl -sIL`). ETSI docbox note: **plain curl default UA gets 403; a browser User-Agent gets 200** — fetch scripts must set `-A "Mozilla/5.0 ..."`.

## 1. Echo / AEC test sets

### Microsoft AEC-Challenge (github.com/microsoft/AEC-Challenge) — VERIFIED, per-file LFS URLs work
- **Contents** (dirs confirmed via GitHub API): `datasets/real`, `real_doubled`, `real_doubled_fullband`, `real_doubled_fullband_mobile`, `synthetic` (`echo_signal/ farend_speech/ nearend_mic_signal/ nearend_speech/ + meta.csv`), `synthetic_fullband`, `test_set`, `test_set_interspeech2021`, `test_set_icassp2022`, `blind_test_set` (`clean/ noisy/`), `blind_test_set_interspeech2021`, `blind_test_set_icassp2022`, `blind_test_set_icassp2023` (`doubletalk/ farend-singletalk/ nearend-singletalk/`, incl. `*_enrl.wav` enrollment for personalized AEC), `RIRs`.
- **Verified LFS media URL pattern**: `https://media.githubusercontent.com/media/microsoft/AEC-Challenge/main/<path>` — e.g. `datasets/real/-2jLGNCgf0WDpKMY2iup7g_doubletalk_mic.wav` (200, 392,684 B); `datasets/blind_test_set_icassp2023/doubletalk/00t5hG031EmZTpJtcnhS0g_doubletalk_mic.wav` (200, 3.7 MB). Not every GUID has every scenario — enumerate via the GitHub trees API first.
- **SR**: 16 kHz (real/synthetic/blind); `*_fullband` = 48 kHz. **License**: repo MIT; audio composed from LibriVox (PD), AudioSet (CC BY 4.0), Freesound (CC0), DEMAND.
- **ECNR use**: enforced perceptual gate corpus; ICASSP-2023 blind doubletalk (movement + enrollment) is the top untapped subset for double-talk work.
- Adjacent: **OpenSLR SLR28 RIRS_NOISES** `https://www.openslr.org/resources/28/rirs_noises.zip` (200, 1.31 GB, Apache 2.0) — RIRs for synthetic echo paths.

## 2. Car-focused noise

### DEMAND (Zenodo 1227121) — direct HTTPS, no registration, license per Zenodo API: CC BY 4.0
`TCAR_16k.zip` 130.0 MB · `TBUS_16k.zip` 128.9 MB · `TMETRO_16k.zip` 126.6 MB · `STRAFFIC_16k.zip` 118.6 MB · `TCAR_48k.zip` 373.5 MB (all 200). 16 ch × 300 s per environment, 16 kHz native. TCAR already feeds the GB/T gate; TBUS/TMETRO/STRAFFIC extend transport coverage.

### ETSI EG 202 396-1 background noise DB (docbox Open, free, browser UA required) — VERIFIED
`https://docbox.etsi.org/STQ/Open/EG%20202%20396-1%20Background%20noise%20database/Binaural_Signals/` — `Fullsize_Car1_{80,100,130}Kmh`, `Midsize_Car{1,2}_{80,100,130}Kmh` binaural WAVs (~5.8 MB each, 30 s, 48 kHz stereo; 130 km/h file verified 200). The canonical handsfree-testing car noises at three speeds; ideal for SHA-pinned B1 speed scenes. Terms: docbox Open testing use; redistribution unclear.

### ETSI TS 103 224 automotive DB (docbox, same UA caveat) — VERIFIED
`FullSizeCar_{80,100,130}_{handset,handsfree}.wav` (46 MB each, 48 kHz, multichannel array) + Crossroadnoise, SymmetricArray (Roadnoise, Inside_Bus, Inside_Train...). Only free **multichannel** automotive noise found — beamformer (ADR-0004) test material; verify channel count on first use.

### DNS-Challenge 5 noise library (Azure blobs) — no registration, tar bundles only
`datasets_fullband.noise_fullband.audioset_000.tar.bz2` 5.36 GB (200); freesound tar 3.47 GB; `impulse_responses_000.tar.bz2` **265 MB** (the only pin-budget artifact). 48 kHz; car clips need label filtering; mixed CC licenses.

### Rejected / flagged
MUSAN (11 GB single tar, no car focus) · **WHAM! CC BY-NC — license-incompatible** · FSD50K (per-clip license filtering needed, 44.1 kHz, sparse vehicle content) · AudioSet (no hosted audio) · Freesound API (OAuth-walled) · QUT-NOISE CAR (request-based).

## 3. Clean speech for mixing

| Corpus | URL (verified) | Size | SR | License |
|---|---|---|---|---|
| LibriSpeech test-clean | openslr.org/resources/12/test-clean.tar.gz | 347 MB | 16 k | CC BY 4.0 |
| AISHELL-1 (Mandarin) | openslr.org/resources/33/data_aishell.tgz | 15.6 GB | 16 k | Apache 2.0 |
| AISHELL-3 (Mandarin) | openslr.org/resources/93/data_aishell3.tgz | 19.1 GB | 44.1 k | Apache 2.0 |
| VCTK 0.92 | datashare.ed.ac.uk/download/DS_10283_3443.zip | 11.7 GB | 48 k | CC BY 4.0 (not re-verified) |
| MagicData-RAMC | openslr 123 | — | 16 k | **CC BY-NC-ND — no commercial** |
| Common Voice zh-CN | registration-gated | — | 48 k MP3 | CC0 (not re-verified) |

## 4. In-car recorded speech (real cabin)

AVICAR: **dead** (no HTTP response). CU-Move/UTDrive: request-walled. MagicHub ASR-SCCabSC (Mandarin in-vehicle, dual mic, 6.13 h): **CC BY-NC-ND + registration** — internal reference only. **No open, permissive, direct-download in-car Chinese corpus exists** — synthesized cabin conditions (P.501 speech + DEMAND/ETSI noise + RIRs) remain the license-clean path.

## 5. Standard test signals

- ITU-T P.501 (04/25) Recommendation PDF: free, verified. The Test Signal Database itself sits behind ITU MyWorkspace login — plan a manual/browser step; old direct-ZIP URL patterns now 500.
- **ETSI TS 103 281 wave files (docbox, verified)**: Annex C test vectors (C01–C08, ~1.1 MB each) and Annex E speech incl. `FBMandarin_QCETSI_26dB.wav` (7.7 MB) — free **Mandarin** P.835-style convergence speech, the standards-grade China-market near-end source.
- ITU-T P.50 App I artificial voices: no free download found.

## Top 5 recommended additions (ranked)

1. ETSI EG 202 396-1 car noises — 9 files × ~5.8 MB, three speeds: pinned B1 speed scenes for the GB/T generator.
2. AEC-Challenge ICASSP-2023 blind doubletalk (~20 clips incl. `_with_movement_`) — hardest public DT material; AECMOS-only scoring.
3. TS 103 281 Annex E FBMandarin (7.7 MB) — Mandarin near-end speech for GB/T conditions.
4. DEMAND TBUS/TMETRO/STRAFFIC 16k — transport-noise breadth, same manifest workflow.
5. TS 103 224 FullSizeCar handsfree multichannel — beamformer/multi-mic test input (ADR-0004).
