
ADR-0005: Render-tap policy — post-software-DRC/EQ, pre-hardware-amp
Status: Accepted
Date: 2026-05-10
Supersedes assumption A4 in: ADR-0001

Context
Per ADR-0001 A4, the render-tap point is the #1 production failure mode in AEC pipelines: any signal processing applied after the tap creates a non-linear discrepancy between AEC's reference and what the speaker actually emits, and linear AEC cannot model that gap. For U300 we own the full software stack — including the audio routing, mixer, software DRC/EQ, and the integration with the smart-amp / codec layer. This converts "what does the HAL expose?" into a design choice.

Render path (canonical, software-stack-owned)
   uplink media (RTP / TTS / nav / music)
        │
        ▼
   [mixer + per-stream gain]
        │
        ▼
   [software DRC / EQ / loudness]   ← we own this in A55 software
        │
        ▼
   ╔═══════════════════╗   ← tap here
   ║  RENDER TAP (ref) ║
   ╚═══════════════════╝
        │
        ▼
   [codec / smart-amp DSP]   ← hardware, vendor IP (TI, Cirrus, etc.)
   [speaker-protection limiter, thermal model, dynamic EQ]
        │
        ▼
   DAC → speaker
Decision
Place the AEC render tap immediately after software DRC/EQ, before the hardware codec / smart-amp DSP.

The tap is a zero-copy mirror of the buffer dispatched to the codec — same int16/float frame, same stride. No re-rendering, no parallel DSP path.

Trade-offs
Tap option	Compute	AEC fidelity	Verdict
Pre-mixer (per source)	Highest	Worst (mixer interactions invisible)	Rejected
Pre-software-DRC	Lower	Poor (DRC/EQ contributes audible level/spectrum changes)	Rejected
Post-software-DRC, pre-hardware-amp	Single ref, zero-copy mirror	Excellent linear AEC; <5 dB hardware-side residual absorbed by neural RES	Chosen
Post-hardware-amp (codec read-back)	High (extra DMA, extra latency, codec must support)	Theoretical best	Rejected — not worth the cost
The chosen tap captures all the processing we control without paying for processing we don't:

Software DRC/EQ/loudness — runs on A55, included in reference.
Smart-amp speaker protection / thermal limiter / dynamic EQ — not in reference, contributes <5 dB non-linear residual on typical automotive amps. Exactly what the neural RES post-filter (NKF-AEC / DTLN-AEC) is designed for.
NXP VoiceSeeker / BdSound Simply Sounds Clear take the same approach on the SAF9100 stack (Cellular Audio Processing Solutions Deep Dive.md:175).

Compute impact
Marginal. The reference frame is the same buffer going to the DAC — no new processing. Render-vs-capture delay is the codec+amp pipeline latency (typically 5–15 ms on automotive smart-amp paths); measured once at HAL bring-up via a loopback impulse, then reported to AEC via SetStreamDelayMs. Render ring memory: ~190 KB at 48 kHz × 1 s × 4 bytes — negligible.

Codebase impact (Phase 1, not Phase 0.5)
src/hal/ gains a RenderTap interface (Phase 1 deliverable):

namespace ecnr::hal {
class RenderTap {
 public:
  virtual ~RenderTap() = default;
  // Called after software DRC/EQ, before the codec/amp DSP, with the same
  // buffer about to be dispatched to the DAC. Forwards to AecChain::ProcessRender.
  virtual void OnRenderFrame(const Frame& f) = 0;
};
}
The contract — "post software DRC/EQ, pre hardware amp" — is part of the interface. A HAL that violates it is broken, not an alternative configuration.

AecChain::ProcessRender (already implemented) is policy-neutral and survives this ADR unchanged. The existing ecnr_live miniaudio path approximates the policy correctly: the buffer it captures is the OS audio buffer; macOS adds its own kernel-side processing afterwards, analogous to the smart-amp.

Action items
done
Phase 0.5 plan: no change — existing AecChain::ProcessRender(Frame) is correct for this policy.
not done
Phase 1: implement hal::RenderTap; U300 audio HAL delivers the post-DRC/EQ buffer.
not done
Phase 1 calibration: render→capture loopback delay measurement (impulse method) → seed SetStreamDelayMs.
not done
Phase 1 measurement: smart-amp residual at typical playback levels → feeds ADR-0007 (neural runtime) sizing.
Open caveats
"<5 dB hardware-side residual" is based on smart-amp design norms (TI TAS, Cirrus CS35Lxx). Validate empirically once U300 HW is locked. If residual is larger, mitigation is post-filter aggressiveness, not a different tap.
If U300 ever moves speaker-protection into A55 software, the tap must move after that layer. Future ADR if/when.
