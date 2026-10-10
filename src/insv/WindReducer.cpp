// SPDX-License-Identifier: MIT OR Apache-2.0
#include "WindReducer.h"
#include "Rdft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace insv {

namespace {

constexpr int kFft = 8192;          // filter STFT: 5.9 Hz bins at 48 kHz
constexpr int kHop = kFft / 4;      // sqrt-Hann in and out, 75 % overlap
constexpr int kBins = kFft / 2 + 1;
constexpr int kTrackFft = 16384;    // engine tracking: 2.9 Hz bins
constexpr int kTrackBins = kTrackFft / 2 + 1;
// The first frame starts this far before sample 0, so sample 0 already gets
// the full overlap-add.
constexpr int64_t kFirstFrame = -(kFft - kHop);
// A frame needs the tracker window around its centre: input up to
// start + kFft/2 + kTrackFft/2. Samples [start, start + kHop) are final once
// that frame is done, hence the delay.
constexpr int kLatency = kFft / 2 + kTrackFft / 2;

constexpr float kMaxCutDb = 24.0f;
constexpr float kTrackMaxHz = 1500.0f; // harmonics the tracker sums
constexpr float kProtectMaxHz = 4000.0f;

// Wind-band weight: 1 below 800 Hz, cosine fade to 0 at 3 kHz.
float windBand(float hz)
{
    if (hz <= 800.0f)
        return 1.0f;
    if (hz >= 3000.0f)
        return 0.0f;
    return 0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * (hz - 800.0f) / 2200.0f);
}

} // namespace

struct WindReducer::Impl {
    const int rate;
    const int channels;
    const float binHz;
    const float trackBinHz;

    Rdft tx{kFft, true};
    Rdft trackTx{kTrackFft, false};
    const std::vector<float> win = sqrtHann(kFft);
    const std::vector<float> trackWin = sqrtHann(kTrackFft);

    // Input, per channel plus the channel average, from absolute frame
    // `inBase` on; `inCount` frames pushed in total.
    std::vector<std::vector<float>> in;
    std::vector<float> mono;
    int64_t inBase = 0;
    int64_t inCount = 0;
    int64_t nextFrame = kFirstFrame;

    // Overlap-add accumulator for [nextFrame, nextFrame + kFft), per channel.
    std::vector<std::vector<float>> acc;
    // Finished output, interleaved, from absolute frame `readyBase` on.
    std::vector<float> ready;
    size_t readyPos = 0; // floats already handed out
    int64_t outCount = 0; // frames handed out (the leading silence included)

    // Per-bin state.
    std::vector<float> floorP, gainSm, protHold, prot, power, gain;
    std::vector<std::vector<AVComplexFloat>> spec;
    // Tracker scratch + state.
    std::vector<float> trackP, logP, env;
    float prevF0 = 0.0f;

    Impl(int r, int ch)
        : rate(r)
        , channels(std::max(1, ch))
        , binHz(static_cast<float>(r) / kFft)
        , trackBinHz(static_cast<float>(r) / kTrackFft)
    {
        in.assign(channels, {});
        acc.assign(channels, std::vector<float>(kFft, 0.0f));
        spec.assign(channels, std::vector<AVComplexFloat>(kBins));
        floorP.assign(kBins, 0.0f);
        gainSm.assign(kBins, 1.0f);
        protHold.assign(kBins, 0.0f);
        prot.assign(kBins, 0.0f);
        power.assign(kBins, 0.0f);
        gain.assign(kBins, 1.0f);
        trackP.assign(kTrackBins, 0.0f);
        const int trackUsed = std::min(kTrackBins, static_cast<int>((kTrackMaxHz + 100.0f) / trackBinHz) + 2);
        logP.assign(trackUsed, 0.0f);
        env.assign(trackUsed, 0.0f);
    }

    void reset()
    {
        for (auto& c : in)
            c.clear();
        mono.clear();
        inBase = 0;
        inCount = 0;
        nextFrame = kFirstFrame;
        for (auto& a : acc)
            std::fill(a.begin(), a.end(), 0.0f);
        ready.clear();
        readyPos = 0;
        outCount = 0;
        std::fill(floorP.begin(), floorP.end(), 0.0f);
        std::fill(gainSm.begin(), gainSm.end(), 1.0f);
        std::fill(protHold.begin(), protHold.end(), 0.0f);
        prevF0 = 0.0f;
    }

    float inputAt(const std::vector<float>& v, int64_t i) const
    {
        const int64_t k = i - inBase;
        return (k >= 0 && k < static_cast<int64_t>(v.size())) ? v[static_cast<size_t>(k)] : 0.0f;
    }

    // The engine fundamental (0 = none) and a 0..1 confidence, from the
    // kTrackFft power spectrum in trackP. Harmonic *sum* of the whitened log
    // spectrum: the true f0 collects every harmonic peak, a multiple of it
    // only some, and a sub-multiple adds the (negative) valleys too.
    std::pair<float, float> trackF0()
    {
        const int nb = static_cast<int>(logP.size());
        for (int k = 0; k < nb; ++k)
            logP[k] = 10.0f * std::log10(trackP[k] + 1e-12f);
        const int half = std::max(2, static_cast<int>(30.0f / trackBinHz)); // ~60 Hz envelope
        double run = 0.0;
        int lo = 0, hi = -1;
        for (int k = 0; k < nb; ++k) {
            while (hi < std::min(nb - 1, k + half))
                run += logP[++hi];
            while (lo < k - half)
                run -= logP[lo++];
            env[k] = static_cast<float>(run / (hi - lo + 1));
        }
        auto white = [&](float f) {
            const float b = f / trackBinHz;
            const int k0 = static_cast<int>(b);
            if (k0 + 1 >= nb)
                return 0.0f;
            const float fr = b - k0;
            const float w = (1 - fr) * (logP[k0] - env[k0]) + fr * (logP[k0 + 1] - env[k0 + 1]);
            return std::clamp(w, -6.0f, 20.0f);
        };
        float best = -1e9f, bestF = 0.0f;
        for (float f0 = 18.0f; f0 <= 250.0f; f0 += 0.1f) {
            float s = 0.0f;
            for (float f = f0; f < kTrackMaxHz; f += f0)
                s += white(f);
            if (prevF0 > 0.0f) // continuity: jumps cost, per octave
                s -= 20.0f * std::fabs(std::log2(f0 / prevF0));
            if (s > best) {
                best = s;
                bestF = f0;
            }
        }
        // Confidence: mean whitened level on the chosen harmonics -- ~1-2 dB
        // for noise (the best of many candidates), 4-5 dB for a clear comb.
        // Noise can pass the 0.2 bar and get protected; that errs towards
        // keeping sound (the camera mic's engine only reaches ~0.3-0.4).
        int count = 0;
        float sum = 0.0f;
        for (float f = bestF; f < kTrackMaxHz; f += bestF, ++count)
            sum += white(f);
        const float conf = std::clamp((sum / std::max(1, count) - 1.0f) / 4.0f, 0.0f, 1.0f);
        if (conf <= 0.2f)
            return {0.0f, conf};
        prevF0 = prevF0 > 0.0f ? 0.5f * prevF0 + 0.5f * bestF : bestF;
        return {bestF, conf};
    }

    // prot[k] in 0..1: a Gaussian around each harmonic of f0, `weight` high.
    void protection(float f0, float weight)
    {
        std::fill(prot.begin(), prot.end(), 0.0f);
        if (f0 <= 0.0f || weight <= 0.0f)
            return;
        for (int h = 1; h * f0 < kProtectMaxHz; ++h) {
            const float c = h * f0 / binHz;
            const float sigma = std::max(0.6f, h * 0.003f * f0 / binHz);
            const int b0 = std::max(0, static_cast<int>(c - 3 * sigma));
            const int b1 = std::min(kBins - 1, static_cast<int>(c + 3 * sigma) + 1);
            for (int b = b0; b <= b1; ++b) {
                const float d = (b - c) / sigma;
                prot[b] = std::max(prot[b], weight * std::exp(-0.5f * d * d));
            }
        }
    }

    void processFrame(int64_t start, float strength)
    {
        // Analysis + per-channel spectra.
        std::fill(power.begin(), power.end(), 0.0f);
        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < kFft; ++i)
                tx.real[i] = inputAt(in[c], start + i) * win[i];
            tx.forward();
            std::copy(tx.cplx, tx.cplx + kBins, spec[c].begin());
            for (int k = 0; k < kBins; ++k)
                power[k] += spec[c][k].re * spec[c][k].re + spec[c][k].im * spec[c][k].im;
        }
        for (float& p : power)
            p /= static_cast<float>(channels);

        // Engine tracking on the channel average, centred on this frame.
        const int64_t tStart = start + kFft / 2 - kTrackFft / 2;
        for (int i = 0; i < kTrackFft; ++i)
            trackTx.real[i] = inputAt(mono, tStart + i) * trackWin[i] * trackWin[i];
        trackTx.forward();
        const int used = static_cast<int>(logP.size());
        for (int k = 0; k < used; ++k)
            trackP[k] = trackTx.cplx[k].re * trackTx.cplx[k].re + trackTx.cplx[k].im * trackTx.cplx[k].im;
        const auto [f0, conf] = trackF0();
        // Full protection from a moderately confident f0 up: scaling it by the
        // confidence itself leaked the cut into the harmonics (spike).
        protection(f0, std::clamp(conf * 2.5f, 0.0f, 1.0f));

        const float frameSec = static_cast<float>(kHop) / rate;
        const float upA = std::exp(-frameSec / 2.0f);   // floor rises over ~2 s
        const float downA = std::exp(-frameSec / 0.2f); // ... falls in ~0.2 s
        const float atkA = std::exp(-frameSec / 0.04f); // gain: cut in ~40 ms
        const float relA = std::exp(-frameSec / 0.15f); // ... recover in ~150 ms
        const float holdA = std::exp(-frameSec / 0.2f); // protection held ~0.2 s
        const float minGain = std::pow(10.0f, -kMaxCutDb / 20.0f);
        // The gust cut scales down with the strength below the default, so 0
        // really is "no change".
        const float gustScale = std::min(1.0f, strength / WindReducer::kDefaultStrength);

        for (int k = 0; k < kBins; ++k) {
            protHold[k] = std::max(prot[k], protHold[k] * holdA);
            const float p = power[k];
            if (floorP[k] == 0.0f)
                floorP[k] = p;
            const float a = p > floorP[k] ? upA : downA;
            floorP[k] = a * floorP[k] + (1 - a) * p;

            float g = 1.0f;
            const float band = windBand(k * binHz);
            if (band > 0.0f) {
                const float gust = std::clamp(1.0f - 2.0f * floorP[k] / (p + 1e-12f), 0.0f, 1.0f);
                const float wind = band * std::max(strength, gust * gustScale);
                g = std::max(1.0f - wind * (1.0f - protHold[k]), minGain);
            }
            const float a2 = g < gainSm[k] ? atkA : relA;
            gainSm[k] = a2 * gainSm[k] + (1 - a2) * g;
        }

        // Synthesis: same gains on every channel, overlap-add.
        for (int c = 0; c < channels; ++c) {
            for (int k = 0; k < kBins; ++k) {
                tx.cplx[k].re = spec[c][k].re * gainSm[k];
                tx.cplx[k].im = spec[c][k].im * gainSm[k];
            }
            tx.inverse();
            // sqrt-Hann twice at 75 % overlap sums to 2.
            float* a = acc[c].data();
            for (int i = 0; i < kFft; ++i)
                a[i] += tx.real[i] * win[i] * 0.5f;
        }

        // [start, start + kHop) is final: hand it on (from sample 0), shift.
        const int64_t from = std::max<int64_t>(start, 0);
        for (int64_t t = from; t < start + kHop; ++t)
            for (int c = 0; c < channels; ++c)
                ready.push_back(acc[c][static_cast<size_t>(t - start)]);
        for (auto& a : acc) {
            std::memmove(a.data(), a.data() + kHop, sizeof(float) * (kFft - kHop));
            std::fill(a.end() - kHop, a.end(), 0.0f);
        }
    }

    void push(const float* pcm, int frames)
    {
        for (int i = 0; i < frames; ++i) {
            float m = 0.0f;
            for (int c = 0; c < channels; ++c) {
                const float v = pcm[static_cast<size_t>(i) * channels + c];
                in[c].push_back(v);
                m += v;
            }
            mono.push_back(m / static_cast<float>(channels));
        }
        inCount += frames;
    }

    void run(float strength)
    {
        while (inCount >= nextFrame + kLatency) {
            processFrame(nextFrame, strength);
            nextFrame += kHop;
        }
        // Drop input no later frame reads (its tracker window starts
        // kTrackFft/2 - kFft/2 before it).
        const int64_t keepFrom = nextFrame - (kTrackFft / 2 - kFft / 2);
        const int64_t drop = keepFrom - inBase;
        if (drop > 4 * kFft) {
            for (auto& c : in)
                c.erase(c.begin(), c.begin() + drop);
            mono.erase(mono.begin(), mono.begin() + drop);
            inBase += drop;
        }
    }

    void pop(float* out, int frames)
    {
        const size_t ch = static_cast<size_t>(channels);
        for (int i = 0; i < frames; ++i, ++outCount) {
            float* o = out + static_cast<size_t>(i) * ch;
            if (outCount < kLatency || readyPos + ch > ready.size()) {
                std::fill(o, o + ch, 0.0f); // the leading delay
                continue;
            }
            std::copy_n(ready.data() + readyPos, ch, o);
            readyPos += ch;
        }
        if (readyPos > ready.size() / 2 && readyPos > 1 << 16) {
            ready.erase(ready.begin(), ready.begin() + static_cast<std::ptrdiff_t>(readyPos));
            readyPos = 0;
        }
    }
};

WindReducer::WindReducer(int sampleRate, int channels)
    : m_impl(std::make_unique<Impl>(sampleRate, channels))
{
}

WindReducer::~WindReducer() = default;

void WindReducer::setStrength(float strength)
{
    m_strength = std::clamp(strength, 0.0f, 1.0f);
}

void WindReducer::reset()
{
    m_impl->reset();
}

int WindReducer::latency() const
{
    return kLatency;
}

void WindReducer::process(const float* in, float* out, int frames)
{
    if (!m_impl->tx.ok() || !m_impl->trackTx.ok()) { // no FFT (never seen): silence
        std::fill(out, out + static_cast<size_t>(frames) * m_impl->channels, 0.0f);
        return;
    }
    m_impl->push(in, frames);
    m_impl->run(m_strength);
    m_impl->pop(out, frames);
}

} // namespace insv
