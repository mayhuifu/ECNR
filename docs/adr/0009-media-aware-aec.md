# ADR-0009: Media-aware AEC — render-type-hint policy

**Status:** Accepted (provisional — implementation gated on Phase 1)
**Date:** 2026-05-10
**Supersedes assumption A5 in:** [ADR-0001](0001-hybrid-aec-architecture-review.md)
**Renumbering note:** This decision was originally reserved as **ADR-0006** in ADR-0001's action-item list. On 2026-05-10 (commit `ae0dbdf`) the 0006 slot was repurposed for the AecChain interface alignment ADR; the media-aware decision was bumped to 0009. See [ADR-0006](0006-aec-chain-interface.md) for the slot it now occupies.

## Context

The Phase 0.5 chain treats the render path as a single opaque mono `Frame`. `AecChain::ProcessRender(const Frame&)` ([ADR-0006 §3](0006-aec-chain-interface.md)) does not know whether the buffer carries:

- incoming far-end voice (RTP / VoNR uplink),
- TTS or navigation prompts synthesised on-device,
- music streamed through the same speakers, or
- a mix (e.g. TTS ducked over music).

ADR-0001 open assumption **A5** flagged this. The linear stage (AEC3) is largely indifferent — adaptive cancellation is correlation-driven and works on music as well as voice — but the *neural post-filter* (RES + RNN-based NS, ADR-0001 stage 2) is trained on voice statistics. Music has long-decay, full-band, high-crest-factor content; a post-filter trained on voice residuals is liable to:

- treat sustained tonal content (sustained bowed strings, organ, synth pads) as residual echo and chase it with non-stationary suppression,
- introduce musical-noise artefacts on the uplink during TTS-while-music ducking,
- chop musical brilliance above ~6 kHz when NS aggressiveness is tuned for voice intelligibility.

[ADR-0003](0003-canonical-sample-rate.md) made this concrete by adopting a two-tier sample-rate strategy: 16 kHz baseline (voice-only, AMR-WB / EVS-WB), 48 kHz fullband (EVS-FB, AEC against music / media). The 16 kHz tier never sees music — by ADR-0003 it is voice-only by definition. **The 48 kHz tier is the one where the render-type question matters.**

Vendor precedent for media-awareness:

- **BdSound *Simply Sounds Clear*** (`Cellular Audio Processing Solutions Deep Dive.md:175, :182`) is positioned as automotive-cabin-aware: AEC at sample rates up to 48 kHz "perfectly aligning with the 3GPP EVS Fullband specifications", paired with AI-based non-stationary noise suppression that distinguishes structural cabin noise (speed-bump impulses, road non-stationarity) from speech and from media. BdSound's "Microphone Bubbles" feature uses spatial isolation to project a virtual barrier around a designated speaker — a concept that only makes sense when the system knows what the renderer is producing vs. what the cabin is producing.
- **NXP VoiceSeeker / SAF9100** (`Cellular Audio Processing Solutions Deep Dive.md:154, :171, :179`) executes AEC, beamforming and delay resolution as a coordinated suite. The SAF9100's design explicitly assumes simultaneous AEC + spatial rendering + road-noise cancellation — i.e. the front end runs while media is playing, not as alternating modes.
- **Cellular research consensus** (`deep-research-report.md:179`) lists "AGC, DRC, EQ, speaker protection placed outside the reference path" as a primary failure mode. Not media-awareness directly, but the same general lesson: the chain has to know what's coming out of the speaker, not just that *something* is.

The conclusion from these is not "build a music-specific AEC". It is: **the post-filter behaviour should be parameterised on what the renderer is producing, and that information has to flow down from the audio HAL — the chain cannot reliably re-derive it from samples alone.**

## Decision

Adopt **Option B — Render-type hint via metadata**.

The HAL passes a `RenderType` enum alongside each render frame. The chain switches the post-filter operating point based on the hint. Default `RenderType::kVoice` preserves Phase 0.5 behaviour for callers that don't supply a hint.

Rationale, in one paragraph: Option A (current — render-agnostic) leaves a known-audible failure mode unaddressed once the 48 kHz tier exercises media playback; ADR-0001's A5 was explicit that it is "probably wrong" and the BdSound positioning confirms vendors solve it. Option C (auto-classify the render frame) is research-grade, adds a classifier on a hot path, can mis-fire (a TTS voice over a music bed reads as "mixed" but the classifier sees only the mixed buffer), and duplicates information the system already has — the renderer or audio framework knows what it's playing. Option B costs one enum on the existing render call, defaults safely, and lets U300's audio framework (which already routes media vs. communication streams differently) be the source of truth.

## RenderType enum

```cpp
namespace ecnr {

enum class RenderType : uint8_t {
  kUnknown = 0,  // Default for callers that don't supply a hint.
                 // Treated as kVoice (safe default — current Phase 0.5 behaviour).
  kVoice   = 1,  // Incoming far-end voice (RTP / VoNR uplink, VoIP downlink).
  kTts     = 2,  // On-device TTS / navigation prompt. Voice statistics, may be louder
                 // and more deterministic than incoming voice.
  kMusic   = 3,  // Full-band media playback. Long decay, sustained tones, high crest
                 // factor.
  kMixed   = 4,  // Two or more of the above active simultaneously (e.g. TTS over a
                 // ducked music bed). Behaviour: most-conservative-of-contributors —
                 // i.e. the union of suppression bypasses, not the intersection.
};

}  // namespace ecnr
```

### Per-type post-filter behaviour matrix

| `RenderType` | Linear AEC (AEC3) | Neural RES (Phase 3) | RNN-based NS (RNNoise) | DTD posture | Notes |
|---|---|---|---|---|---|
| `kUnknown` | enabled, default config | as `kVoice` | as `kVoice` | as `kVoice` | Safe-default fallback. |
| `kVoice` | enabled, default config | enabled, default suppression aggressiveness | enabled, default aggressiveness | nominal | Current Phase 0.5 behaviour. Reference baseline. |
| `kTts` | enabled, default config | enabled, default | enabled, default | **stronger DTD protection** — TTS is loud and locally-generated, so the false-positive rate for "user is also speaking" is higher; bias DTD toward preserving near-end | TTS comes from a known buffer with known levels; the chain can be more confident the linear stage will cancel it well. |
| `kMusic` | enabled, default config | **lower suppression floor** (cap aggressive non-stationary suppression at -6 dB rather than -20 dB; do not gate full bands) | **bypassed for the duration** | relaxed (driver does not typically over-talk music as aggressively as voice) | Linear AEC still runs — AEC3 cancels music fine, the linear correlation does not care about content. The post-filter is the part that needs to back off. RNNoise gates music as "noise" with high confidence; bypassing it is the cleanest fix. |
| `kMixed` | enabled, default config | **most-conservative contributor** (e.g. TTS+Music → use `kMusic`'s lower floor, not `kVoice`'s floor) | **bypassed if any contributor is `kMusic`** | nominal (TTS dominates the DTD reasoning) | Pick by union, not intersection: if any contributor would have its suppression backed off, back it off. Avoids switching back into aggressive NS in the middle of a TTS prompt that's playing over music. |

The matrix is normative for the policy; the exact numeric thresholds (the "-6 dB cap", "default aggressiveness") are *placeholders* until measured in Phase 1 (see Open assumptions).

## Interface change

### `AecChain` surface delta

Two changes to the [ADR-0006](0006-aec-chain-interface.md) sketch, both additive, neither breaking:

1. **`ProcessRender` gains an optional render-type argument**, defaulted so existing callers compile unchanged:

   ```cpp
   // Was (ADR-0006 §3):
   void ProcessRender(const Frame& render);

   // Now:
   void ProcessRender(const Frame& render, RenderType type = RenderType::kUnknown);
   ```

   The default-argument form is chosen over a separate `SetRenderType` setter because:

   - Render type is a *property of this frame*, not chain-global state. A music track that ends at sample N and is followed by silence and then a TTS prompt at sample N+4800 should not require the caller to interleave `SetRenderType` calls between `ProcessRender` calls; the type rides the frame.
   - It avoids the lifetime question "what RenderType is in effect right now?" if `SetRenderType` and `ProcessRender` are called from different threads.
   - It mirrors how WebRTC APM accepts per-call hints (`set_stream_delay_ms`, `set_stream_analog_level`) — except those are per-capture, and this one is per-render.

   **Rejected alternative:** carrying `RenderType` *inside* `Frame` itself. Rejected because `Frame` is a transport for samples; loading it with non-sample metadata invites scope creep (next someone wants `is_silence`, `stream_id`, `timestamp_ns`...) and forces the capture path to carry a field that has no meaning on capture.

2. **`ChainStats` gains a `render_type` field** — what the chain *currently believes* the renderer is producing, for telemetry and for tests:

   ```cpp
   // Added to ChainStats (extension of ADR-0006 §3 sketch):
   //
   // Most recent RenderType seen on ProcessRender. Useful for telemetry
   // (verifying the HAL is actually labelling music as kMusic) and for tests
   // that want to assert post-filter mode followed the hint. Updated atomically
   // with each ProcessRender call.
   RenderType render_type = RenderType::kUnknown;
   ```

   This is the only `ChainStats` extension Phase 1 needs; the per-mode post-filter parameters live in the chain's internal state and are not surfaced (consistent with ADR-0006's "we own the config" stance on `Config::NoiseSuppression` etc.).

### What does *not* change

- `ProcessCapture` signature — no `RenderType` on capture. The chain remembers the last render type it saw and applies it on the matching capture frame. (This is the same temporal pairing AEC3 already uses for delay alignment; we piggy-back on it.)
- `Reset()` semantic — resets to `RenderType::kUnknown` (≡ `kVoice` behaviour), consistent with "drop adapted state".
- The 16 kHz tier — the hint is *accepted* on the 16 kHz interface but is informational only. By ADR-0003 the 16 kHz tier is voice-only; the post-filter does not switch modes there. A HAL that labels a 16 kHz frame `kMusic` is reporting a HAL bug, not configuring a mode.

## Options considered

| Option | Mechanism | Pros | Cons | Verdict |
|---|---|---|---|---|
| **A. Render-type-agnostic** | Chain treats render uniformly; post-filter tuned for voice; live with whatever artefacts media produces. | Zero interface change. Smallest implementation. | Audible artefacts on music (RNNoise gates sustained tones; neural RES chases tonal residuals). ADR-0001 A5 explicitly flagged this. Requires a re-architecture later under field pressure. | **Rejected.** |
| **B. Render-type hint via metadata** *(chosen)* | HAL passes `RenderType` enum on `ProcessRender`. Chain switches post-filter mode. Defaults to `kUnknown`/`kVoice`. | One-enum interface change, fully default-compatible. The renderer/HAL already knows what it's playing — uses authoritative information, not derived. Matches BdSound / NXP precedent of treating media-awareness as a system property, not a signal-processing puzzle. Reversible: a HAL that doesn't supply the hint stays on Phase 0.5 behaviour. | Pushes a tiny bit of policy into the HAL contract — the HAL must label its render streams. For U300 this is plausible because we own the audio framework (per ADR-0005's "we own the full software stack"). | **Chosen.** |
| **C. Auto-classify** | Chain inspects the render frame (spectral entropy, tonality, VAD on render) and classifies inline. | No HAL contract change. Works on third-party HALs that don't label streams. | Adds a classifier on the hot path. Misclassifies mixed content (TTS-over-music reads as "mixed" but the classifier sees one buffer). Needs training data / validation. Duplicates information the system already has authoritatively. Brittle in exactly the cases that matter (sustained organ note vs. sustained vowel — spectrally similar, behaviourally different). | **Rejected** as the primary mechanism. May become a *fallback* for HALs that don't supply the hint, in a much later phase. |

## Trade-off analysis

The deciding axis is **where authoritative information lives**. Option C tries to recover, from samples, information that the source already has — that's strictly worse than asking the source. Option A pretends the information doesn't matter — it does, per A5. Option B is the only one that puts the policy decision (which post-filter mode to run) at the layer that has the data (the audio framework / HAL knows it just routed a music stream). The cost is one enum on one method and a per-mode parameter table that has to be measured in Phase 1.

Risk that B is wrong: the HAL doesn't actually have the information either — e.g. an Android-style audio HAL that gets a mixed PCM buffer with no per-stream metadata. For U300 we own the framework above the HAL (ADR-0005) so this is recoverable; for a future port to a HAL we don't own, fallback to Option C is on the table. The interface is designed so that fallback is internal (the chain runs a classifier and synthesises a `RenderType`) without the public surface changing.

## Consequences

**What becomes easier:**
- The 48 kHz tier (ADR-0003 fullband) gets a defined story for "what does the post-filter do during music playback" before any music-quality regression hits the field.
- Telemetry: `ChainStats::render_type` is observable in tests and on-device, so we can verify the HAL is labelling streams correctly without instrumenting the HAL.
- Per-mode post-filter parameters become measurable, sweepable, and tunable separately from the voice baseline. The voice baseline is *not* affected by the music tuning, which is the whole point.

**What becomes harder:**
- The HAL contract grows by one enum. A HAL written without media-awareness (a pre-Phase-1 mock, a third-party port) will land on `kUnknown` → `kVoice` behaviour, which is correct but means the field benefit only materialises once the U300 HAL is wired with stream-type routing.
- The post-filter has more states. Each state needs its own regression suite. Phase 3 (neural RES integration) takes on a concrete obligation: the model must behave acceptably under the `kMusic` operating point, not just the `kVoice` one. This is added scope.
- A `kMixed` policy that picks the most-conservative contributor can occasionally under-suppress (e.g. a quiet music bed under a loud TTS prompt — the chain backs off NS because of the music, even though the music is inaudible to the user). Acceptable: under-suppression of a faint music bed is a quality-neutral failure mode, vs. over-suppression of music which is audible.

**What we'll need to revisit:**
- The numeric thresholds in the per-type matrix (the "-6 dB suppression floor for music") are placeholders. Phase 1 measurement closes them out.
- Whether U300's audio framework can label streams at the `RenderType` granularity. If it cannot (e.g. it gives us a single mixed PCM bus with no metadata), Option C fallback ships as the chain-internal classifier.
- The interaction with ADR-0005's render-tap point. The tap is post-software-DRC/EQ; if the DRC/EQ is mode-dependent (e.g. different EQ for music vs voice), then `RenderType` and the tap policy are coupled and an addendum to either ADR may be needed.

## Open assumptions

1. **Magnitude of the music-as-echo quality regression if we ship Option A is unmeasured.** ADR-0001 A5 states it's "probably" a problem; we have no on-device numbers. Phase 1 measurement: run the 48 kHz tier with music as the render signal, voice as capture, and compare uplink quality (PESQ, SDR, listening test) under (a) Phase-0.5 voice-tuned post-filter vs. (b) post-filter bypassed. If the gap is < 0.2 PESQ, the urgency of this ADR drops; if > 0.5, it becomes blocking.
2. **Whether U300 HAL will provide the metadata.** Open per ADR-0001's "HAL conversation with U300 platform" action item. If yes → Option B is straightforward. If no → fallback to Option C as a chain-internal classifier; the public interface (`ProcessRender(Frame, RenderType=kUnknown)`) is unchanged.
3. **The exact suppression-floor numbers** (`-6 dB cap` for `kMusic`, "default aggressiveness" for `kVoice`) are placeholders. Phase 1 sweeps them.
4. **`kMixed` arbitration policy** ("most-conservative contributor"). Plausible default; not validated. If it under-suppresses noticeably under common cabin scenarios (e.g. road noise + TTS + ducked music) we revisit.
5. **DTT (TTS) vs incoming-voice DTD distinction.** We claim TTS warrants stronger DTD protection because levels and timing are known. Untested; folded into Phase 1 measurement.

## Phase 1 entry criteria

Implementation lands when **any** of the following triggers fires:

- The 48 kHz tier (ADR-0003 fullband) is wired into a real device path that exercises media playback simultaneously with hands-free uplink — i.e. the first time a user can stream music and place a call on U300.
- An audible artefact on music-as-echo is reported from internal dogfooding or a customer trial on the 48 kHz tier.
- The neural RES (ADR-0007) integrates into the chain. The RES inherits this policy; landing it without the policy means tuning twice.

Phase 0.5 is voice-only by ADR-0003; **none of the above can trigger during Phase 0.5**. The ADR is a policy commitment recorded now so that Phase 1 / Phase 3 do not re-litigate it.

## Reversibility

If we ship Phase 0.5 without media-awareness (which we *are* doing — Phase 0.5 is voice-only) and discover a music problem in field, the migration to Option B is:

1. Add `RenderType` enum and the defaulted argument to `ProcessRender`. *Existing callers compile unchanged* because of the default. No public API break.
2. Implement the per-mode switch inside `AecChain::Impl`.
3. Wire the U300 audio framework to label streams.

Steps 1 and 2 are chain-local and ship in a single commit. Step 3 is a HAL-side change that lands independently — the chain works correctly with `kUnknown` until the HAL catches up. **No major refactor; no data migration; no caller break.**

This is the property that makes Option B safe to commit to as a policy now while deferring implementation to Phase 1: the cost of being wrong is bounded by "ship Phase 0.5 with no behavioural change, add the enum later".

## Action items

Done:
- **Decision recorded.** This ADR commits the policy: render-type hint via HAL metadata, default-compatible, applied at the post-filter mode switch.
- **Close ADR-0001 A5.** Pointer to this ADR; A5 is no longer an open assumption, it's a decision with deferred implementation.

Not done (Phase 1):
- Implement `RenderType` enum and the defaulted `ProcessRender` argument in [`src/pipeline/aec_chain.h`](../../src/pipeline/aec_chain.h) and `aec_chain.cc` (extends ADR-0006 §3 sketch).
- Add `ChainStats::render_type` (extends ADR-0006 §3 `ChainStats`).
- Per-mode post-filter parameters (the matrix above) — concrete numeric thresholds measured under Phase 1 dogfood conditions.

Not done (Phase 1, HAL side):
- U300 audio framework: label render streams with one of {voice, TTS, music, mixed} at the boundary that calls `ProcessRender`.
- Conversation with U300 platform team confirming the labelling is available (folds into the ADR-0001 "HAL conversation" action item).

Not done (Phase 3):
- Neural RES (ADR-0007) takes the matrix as a hard constraint on its training/eval set: the model must produce acceptable output under `kMusic` *and* `kVoice`, not just `kVoice`.

Conditional (only if Open assumption #2 resolves to "HAL cannot label"):
- Implement Option C fallback: chain-internal render classifier (spectral entropy + render-side VAD) that synthesises a `RenderType` when the caller passes `kUnknown`. Public surface unchanged.

## References

- [ADR-0001 §A5](0001-hybrid-aec-architecture-review.md) — open assumption "Music-as-echo will be handled by the same AEC". Superseded by this ADR.
- [ADR-0003](0003-canonical-sample-rate.md) — two-tier 16 kHz / 48 kHz strategy. The 48 kHz tier is where this policy is load-bearing.
- [ADR-0005](0005-render-tap-policy.md) — render tap is post-software-DRC/EQ. Coupled to this ADR if DRC/EQ ever becomes render-type-dependent.
- [ADR-0006](0006-aec-chain-interface.md) — `AecChain` surface this ADR extends. The renumbering of slot 0006 explained at the top of this file is documented in ADR-0001's action-item list.
- [`Cellular Audio Processing Solutions Deep Dive.md:154-182`](../Cellular%20Audio%20Processing%20Solutions%20Deep%20Dive.md) — NXP VoiceSeeker / SAF9100 + BdSound *Simply Sounds Clear* sections; the "Microphone Bubbles" / 48 kHz EVS-Fullband AEC positioning that this ADR draws on.
- [`deep-research-report.md:179`](../deep-research-report.md) — cellular consensus on reference-path consistency (Chinese counterpart; less automotive-media-specific but reinforces "what's coming out of the speaker has to be known to the chain").
- [`src/pipeline/aec_chain.h`](../../src/pipeline/aec_chain.h) — the interface that will be extended in Phase 1.
