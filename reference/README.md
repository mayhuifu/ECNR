# reference/

Test audio for offline evaluation. Empty for now; populated as Phase 0 bench needs grow and as Phase 2 cabin characterization data lands.

## Phase 0 (synthetic, dev host)

Goal: prove the AEC3 + RNNoise pipeline runs end-to-end with measurable ERLE.

`gen_synth.py` (to be authored) produces:

- `synth/ref.wav` — far-end stimulus (white noise or speech).
- `synth/mic.wav` — `ref.wav` convolved with a short synthetic room impulse response, optionally mixed with clean near-end speech.
- `synth/near_clean.wav` — the unmixed near-end signal (oracle reference).

The pair is time-aligned at sample 0; AEC's delay estimator handles any added delay.

## Phase 2 (cabin)

To be defined. Will include:

- Cabin impulse-response measurements (driver mic ← head-unit speakers).
- Road / wind / HVAC noise at multiple speeds.
- Music + speech double-talk corpus.
- Multi-mic (driver + passenger) recordings.

Not committed to git. Large WAV files are excluded by `.gitignore`; store the canonical corpus on shared storage and mirror locally.
