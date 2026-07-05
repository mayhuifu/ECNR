#include "pipeline/dtln_res_adapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "onnxruntime_cxx_api.h"

namespace ecnr {

namespace {

constexpr int kBlockLen = 512;
constexpr int kBlockShift = 128;
constexpr int kFftBins = kBlockLen / 2 + 1;  // 257
// Selector detector: trailing window (10 ms frames) and max render→output
// lag searched, mirroring the offline prototype in
// docs/phase-3-res-hybrid-notes.md.
constexpr int kLikWindowFrames = 50;   // 500 ms. Shorter windows (300 ms
                                       // tried) read the syllabic-envelope
                                       // similarity of ANY two speech
                                       // signals as echo — near-end frames
                                       // measured lik p50 0.48 and the
                                       // selector starved the neural path.
constexpr int kLikMaxLagFrames = 25;   // 250 ms
constexpr int kLikHistoryFrames = kLikWindowFrames + kLikMaxLagFrames + 2;

// Minimal iterative radix-2 complex FFT, size 512 only. Three transforms
// per 8 ms hop (~14k butterflies total) is noise next to the LSTM GEMMs,
// so a textbook implementation beats dragging in another vendored FFT.
// Verified against known DFT identities in dtln_res_adapter unit usage
// (chain smoke asserts energy conservation via round trip at Init).
class Fft512 {
 public:
  Fft512() {
    for (int i = 0; i < kBlockLen; ++i) {
      int r = 0;
      for (int b = 0; b < 9; ++b) r = (r << 1) | ((i >> b) & 1);
      bitrev_[i] = static_cast<uint16_t>(r);
    }
    for (int i = 0; i < kBlockLen / 2; ++i) {
      const double a = -2.0 * M_PI * i / kBlockLen;
      tw_re_[i] = static_cast<float>(std::cos(a));
      tw_im_[i] = static_cast<float>(std::sin(a));
    }
  }

  // In-place complex FFT. invert=true computes the unscaled inverse
  // (caller divides by N).
  void Transform(float* re, float* im, bool invert) const {
    for (int i = 0; i < kBlockLen; ++i) {
      const int j = bitrev_[i];
      if (j > i) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= kBlockLen; len <<= 1) {
      const int half = len >> 1;
      const int step = kBlockLen / len;
      for (int base = 0; base < kBlockLen; base += len) {
        for (int k = 0; k < half; ++k) {
          const float wr = tw_re_[k * step];
          const float wi = invert ? -tw_im_[k * step] : tw_im_[k * step];
          const int a = base + k;
          const int b = a + half;
          const float xr = re[b] * wr - im[b] * wi;
          const float xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
      }
    }
  }

 private:
  std::array<uint16_t, kBlockLen> bitrev_{};
  std::array<float, kBlockLen / 2> tw_re_{};
  std::array<float, kBlockLen / 2> tw_im_{};
};

struct Stage {
  Ort::Session session{nullptr};
  std::vector<std::string> in_names;
  std::vector<std::string> out_names;
  std::vector<int64_t> state_shape;
  std::vector<float> states;
};

bool LoadStage(Ort::Env& env, const Ort::SessionOptions& opts,
               const std::string& path, Stage* s) {
  try {
    s->session = Ort::Session(env, path.c_str(), opts);
  } catch (const Ort::Exception& e) {
    std::fprintf(stderr, "DtlnResAdapter: cannot load %s: %s\n", path.c_str(),
                 e.what());
    return false;
  }
  Ort::AllocatorWithDefaultOptions alloc;
  const size_t n_in = s->session.GetInputCount();
  const size_t n_out = s->session.GetOutputCount();
  if (n_in != 3 || n_out < 2) {
    std::fprintf(stderr,
                 "DtlnResAdapter: %s has unexpected I/O arity (%zu in, %zu "
                 "out; expected 3/2)\n",
                 path.c_str(), n_in, n_out);
    return false;
  }
  for (size_t i = 0; i < n_in; ++i) {
    s->in_names.push_back(s->session.GetInputNameAllocated(i, alloc).get());
  }
  for (size_t i = 0; i < n_out; ++i) {
    s->out_names.push_back(s->session.GetOutputNameAllocated(i, alloc).get());
  }
  // Input 1 is the LSTM state tensor in both exported stages (positional
  // layout preserved from the upstream TFLite export; see
  // reference/convert_dtln_res.py validation). NB: keep the TypeInfo
  // owner alive while reading the shape — GetTensorTypeAndShapeInfo()
  // returns a view into it, and a dangling view yields an empty shape
  // (manifested as "Invalid rank ... Got: 0" at Run time).
  Ort::TypeInfo type_info = s->session.GetInputTypeInfo(1);
  auto info = type_info.GetTensorTypeAndShapeInfo();
  s->state_shape = info.GetShape();
  int64_t total = 1;
  for (auto d : s->state_shape) total *= d > 0 ? d : 1;
  s->states.assign(static_cast<size_t>(total), 0.0f);
  return true;
}

}  // namespace

struct DtlnResAdapter::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "ecnr_res"};
  Stage s1, s2;
  bool ready = false;

  Fft512 fft;

  // Streaming state (all 16 kHz).
  std::array<float, kBlockLen> in_buf{};    // sliding mic block
  std::array<float, kBlockLen> lpb_buf{};   // sliding loopback block
  std::array<float, kBlockLen> ola_buf{};   // overlap-add accumulator
  std::deque<float> mic_fifo;
  std::deque<float> lpb_fifo;
  std::deque<float> out_fifo;               // primed with latency zeros

  // Selector detector history: per-10 ms log powers.
  std::deque<float> render_logp;
  std::deque<float> out_logp;
  float last_likelihood = 0.0f;

  void PrimeOutput() {
    out_fifo.assign(static_cast<size_t>(kLatencySamples16k), 0.0f);
  }

  void ResetStreaming() {
    in_buf.fill(0.0f);
    lpb_buf.fill(0.0f);
    ola_buf.fill(0.0f);
    mic_fifo.clear();
    lpb_fifo.clear();
    PrimeOutput();
    render_logp.clear();
    out_logp.clear();
    last_likelihood = 0.0f;
    std::fill(s1.states.begin(), s1.states.end(), 0.0f);
    std::fill(s2.states.begin(), s2.states.end(), 0.0f);
  }

  void RunHop() {
    // Shift in 128 new samples per buffer.
    std::memmove(in_buf.data(), in_buf.data() + kBlockShift,
                 (kBlockLen - kBlockShift) * sizeof(float));
    std::memmove(lpb_buf.data(), lpb_buf.data() + kBlockShift,
                 (kBlockLen - kBlockShift) * sizeof(float));
    for (int i = 0; i < kBlockShift; ++i) {
      in_buf[kBlockLen - kBlockShift + i] = mic_fifo.front();
      mic_fifo.pop_front();
      lpb_buf[kBlockLen - kBlockShift + i] = lpb_fifo.front();
      lpb_fifo.pop_front();
    }

    // FFTs of both blocks.
    alignas(16) float mre[kBlockLen], mim[kBlockLen];
    alignas(16) float lre[kBlockLen], lim[kBlockLen];
    std::memcpy(mre, in_buf.data(), sizeof(mre));
    std::memset(mim, 0, sizeof(mim));
    std::memcpy(lre, lpb_buf.data(), sizeof(lre));
    std::memset(lim, 0, sizeof(lim));
    fft.Transform(mre, mim, /*invert=*/false);
    fft.Transform(lre, lim, /*invert=*/false);

    float mic_mag[kFftBins], lpb_mag[kFftBins];
    for (int i = 0; i < kFftBins; ++i) {
      mic_mag[i] = std::sqrt(mre[i] * mre[i] + mim[i] * mim[i]);
      lpb_mag[i] = std::sqrt(lre[i] * lre[i] + lim[i] * lim[i]);
    }

    Ort::MemoryInfo mem =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 3> mag_shape{1, 1, kFftBins};

    // Stage 1: magnitude masking.
    Ort::Value in0 = Ort::Value::CreateTensor<float>(
        mem, mic_mag, kFftBins, mag_shape.data(), mag_shape.size());
    Ort::Value in1 = Ort::Value::CreateTensor<float>(
        mem, s1.states.data(), s1.states.size(), s1.state_shape.data(),
        s1.state_shape.size());
    Ort::Value in2 = Ort::Value::CreateTensor<float>(
        mem, lpb_mag, kFftBins, mag_shape.data(), mag_shape.size());
    const char* in_names1[] = {s1.in_names[0].c_str(), s1.in_names[1].c_str(),
                               s1.in_names[2].c_str()};
    const char* out_names1[] = {s1.out_names[0].c_str(),
                                s1.out_names[1].c_str()};
    std::array<Ort::Value, 3> inputs1{std::move(in0), std::move(in1),
                                      std::move(in2)};
    auto out1 = s1.session.Run(Ort::RunOptions{nullptr}, in_names1,
                               inputs1.data(), 3, out_names1, 2);
    const float* mask = out1[0].GetTensorData<float>();
    {
      const float* ns = out1[1].GetTensorData<float>();
      std::copy(ns, ns + s1.states.size(), s1.states.begin());
    }

    // Apply mask in the frequency domain, back to time.
    for (int i = 0; i < kFftBins; ++i) {
      mre[i] *= mask[i];
      mim[i] *= mask[i];
      if (i > 0 && i < kFftBins - 1) {
        // Maintain conjugate symmetry for the real inverse transform.
        mre[kBlockLen - i] = mre[i];
        mim[kBlockLen - i] = -mim[i];
      }
    }
    fft.Transform(mre, mim, /*invert=*/true);
    float est[kBlockLen];
    for (int i = 0; i < kBlockLen; ++i) est[i] = mre[i] / kBlockLen;

    // Stage 2: time-domain refinement with the loopback block.
    const std::array<int64_t, 3> blk_shape{1, 1, kBlockLen};
    float lpb_time[kBlockLen];
    std::memcpy(lpb_time, lpb_buf.data(), sizeof(lpb_time));
    Ort::Value jn0 = Ort::Value::CreateTensor<float>(
        mem, est, kBlockLen, blk_shape.data(), blk_shape.size());
    Ort::Value jn1 = Ort::Value::CreateTensor<float>(
        mem, s2.states.data(), s2.states.size(), s2.state_shape.data(),
        s2.state_shape.size());
    Ort::Value jn2 = Ort::Value::CreateTensor<float>(
        mem, lpb_time, kBlockLen, blk_shape.data(), blk_shape.size());
    const char* in_names2[] = {s2.in_names[0].c_str(), s2.in_names[1].c_str(),
                               s2.in_names[2].c_str()};
    const char* out_names2[] = {s2.out_names[0].c_str(),
                                s2.out_names[1].c_str()};
    std::array<Ort::Value, 3> inputs2{std::move(jn0), std::move(jn1),
                                      std::move(jn2)};
    auto out2 = s2.session.Run(Ort::RunOptions{nullptr}, in_names2,
                               inputs2.data(), 3, out_names2, 2);
    const float* blk = out2[0].GetTensorData<float>();
    {
      const float* ns = out2[1].GetTensorData<float>();
      std::copy(ns, ns + s2.states.size(), s2.states.begin());
    }

    // Overlap-add; emit one hop of samples.
    std::memmove(ola_buf.data(), ola_buf.data() + kBlockShift,
                 (kBlockLen - kBlockShift) * sizeof(float));
    std::fill(ola_buf.end() - kBlockShift, ola_buf.end(), 0.0f);
    for (int i = 0; i < kBlockLen; ++i) ola_buf[i] += blk[i];
    for (int i = 0; i < kBlockShift; ++i) out_fifo.push_back(ola_buf[i]);
  }

  static float LogPower(const float* x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::log10(s / n + 1e-12));
  }

  // Render and output log-power series advance on the same 10 ms tick
  // cadence (PushRender / ProcessCapture are called once each per tick by
  // AecChain), so lag 0 in the search means "same tick".
  void PushRenderLogPower(float v) {
    render_logp.push_back(v);
    while (render_logp.size() > kLikHistoryFrames) render_logp.pop_front();
  }

  void UpdateLikelihood(const float* out, int n) {
    out_logp.push_back(LogPower(out, n));
    while (out_logp.size() > kLikHistoryFrames) out_logp.pop_front();

    const int nb = static_cast<int>(out_logp.size());
    if (nb < 12) {
      last_likelihood = 0.0f;
      return;
    }
    const int w = std::min(kLikWindowFrames, nb);
    // Copy the trailing window of the output series once.
    std::vector<float> b(out_logp.end() - w, out_logp.end());
    float bm = 0.0f;
    for (float v : b) bm += v;
    bm /= w;
    float bv = 0.0f;
    for (float v : b) bv += (v - bm) * (v - bm);
    if (bv < 1e-6f) return;  // flat output — keep previous likelihood
    float best = 0.0f;
    const int nr = static_cast<int>(render_logp.size());
    for (int d = 0; d <= kLikMaxLagFrames; ++d) {
      if (nr - d < w) break;
      float rm = 0.0f;
      auto it = render_logp.end() - w - d;
      for (int i = 0; i < w; ++i) rm += *(it + i);
      rm /= w;
      float rv = 0.0f, rb = 0.0f;
      for (int i = 0; i < w; ++i) {
        const float r = *(it + i) - rm;
        rv += r * r;
        rb += r * (b[i] - bm);
      }
      if (rv < 1e-6f) continue;
      const float c = rb / std::sqrt(rv * bv);
      if (c > best) best = c;
    }
    last_likelihood = best;
  }
};

DtlnResAdapter::DtlnResAdapter() : impl_(std::make_unique<Impl>()) {}
DtlnResAdapter::~DtlnResAdapter() = default;

bool DtlnResAdapter::Init(int sample_rate_hz, const std::string& model_dir,
                          int units) {
  if (sample_rate_hz != 16000) {
    std::fprintf(stderr,
                 "DtlnResAdapter: only the 16 kHz tier is supported (got %d)\n",
                 sample_rate_hz);
    return false;
  }
  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(1);
  opts.SetInterOpNumThreads(1);
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  char path[512];
  std::snprintf(path, sizeof(path), "%s/dtln_aec_%d_1.onnx",
                model_dir.c_str(), units);
  if (!LoadStage(impl_->env, opts, path, &impl_->s1)) return false;
  std::snprintf(path, sizeof(path), "%s/dtln_aec_%d_2.onnx",
                model_dir.c_str(), units);
  if (!LoadStage(impl_->env, opts, path, &impl_->s2)) return false;
  impl_->ResetStreaming();
  impl_->ready = true;
  return true;
}

void DtlnResAdapter::Reset() {
  if (impl_->ready) impl_->ResetStreaming();
}

void DtlnResAdapter::PushRender(const Frame& render) {
  if (!impl_->ready || render.n_channels != 1) return;
  float buf[kFrameSamples16k];
  const int n = std::min(render.n_samples, kFrameSamples16k);
  for (int i = 0; i < n; ++i) {
    const float v = static_cast<float>(render.ch[0][i]) / 32768.0f;
    impl_->lpb_fifo.push_back(v);
    buf[i] = v;
  }
  impl_->PushRenderLogPower(Impl::LogPower(buf, n));
}

bool DtlnResAdapter::ProcessCapture(const Frame& mic_in, Frame& out) {
  out.n_channels = 1;
  out.n_samples = mic_in.n_samples;
  if (!impl_->ready || mic_in.n_channels != 1 ||
      mic_in.n_samples != kFrameSamples16k) {
    for (int i = 0; i < out.n_samples; ++i) out.ch[0][i] = 0;
    return false;
  }
  for (int i = 0; i < mic_in.n_samples; ++i) {
    impl_->mic_fifo.push_back(static_cast<float>(mic_in.ch[0][i]) / 32768.0f);
  }
  // The render side may briefly lag the capture side (caller pushes render
  // first each tick in AecChain, so steady state is 1:1); zero-fill any
  // shortfall instead of stalling the audio thread.
  while (impl_->lpb_fifo.size() < impl_->mic_fifo.size()) {
    impl_->lpb_fifo.push_back(0.0f);
  }
  while (impl_->mic_fifo.size() >= kBlockShift &&
         impl_->lpb_fifo.size() >= kBlockShift) {
    impl_->RunHop();
  }

  float out_frame[kFrameSamples16k];
  for (int i = 0; i < kFrameSamples16k; ++i) {
    const float v = impl_->out_fifo.front();
    impl_->out_fifo.pop_front();
    out_frame[i] = v;
    const float c = v * 32768.0f;
    out.ch[0][i] = static_cast<int16_t>(
        c < -32768.0f ? -32768 : c > 32767.0f ? 32767 : c);
  }
  impl_->UpdateLikelihood(out_frame, kFrameSamples16k);
  return true;
}

float DtlnResAdapter::LastEchoLikelihood() const {
  return impl_->last_likelihood;
}

}  // namespace ecnr
