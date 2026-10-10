// SPDX-License-Identifier: MIT OR Apache-2.0
#include "GeneralWindReducer.h"
#include "Rdft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace insv {

namespace {

constexpr int kFft = 2048;     // 23 Hz bins, ~43 ms at 48 kHz
constexpr int kHop = kFft / 4; // sqrt-Hann in and out, 75 % overlap
constexpr int kBins = kFft / 2 + 1;
// The first frame starts this far before sample 0, so sample 0 already gets
// the full overlap-add; samples [start, start + kHop) are final once the
// frame at `start` (input up to start + kFft) is done, hence the delay.
constexpr int64_t kFirstFrame = -(kFft - kHop);
constexpr int kLatency = kFft;

constexpr float kFitLoHz = 80.0f;
constexpr float kFitHiHz = 1000.0f;
constexpr float kFadeFromHz = 2000.0f; // the wind estimate fades out
constexpr float kFadeToHz = 4000.0f;   // ... to nothing here
constexpr float kMaxCutDb = 30.0f;     // at strength 1
// The floor is a minimum over ~7 bins of a noisy power estimate, which sits
// below the noise's mean; this brings it back up (measured on steady
// coloured noise, GeneralWindReducer tests in audio_mix_test).
constexpr float kFloorBias = 3.0f;

float fade(float hz)
{
    if (hz <= kFadeFromHz)
        return 1.0f;
    if (hz >= kFadeToHz)
        return 0.0f;
    return 0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * (hz - kFadeFromHz) /
                                  (kFadeToHz - kFadeFromHz));
}

} // namespace

struct GeneralWindReducer::Impl {
    const int rate;
    const int channels;
    const float binHz;
    const int usedBins; // up to kFadeToHz: the gain is 1 above
    const int minHalf;  // floor: minimum over +-minHalf bins (~70 Hz)

    Rdft tx{kFft, true};
    const std::vector<float> win = sqrtHann(kFft);

    // Input per channel from absolute frame `inBase` on; `inCount` frames
    // pushed in total.
    std::vector<std::vector<float>> in;
    int64_t inBase = 0;
    int64_t inCount = 0;
    int64_t nextFrame = kFirstFrame;

    std::vector<std::vector<float>> acc; // overlap-add, per channel
    std::vector<float> ready;            // finished output, interleaved
    size_t readyPos = 0;
    int64_t outCount = 0;

    std::vector<std::vector<AVComplexFloat>> spec;
    std::vector<float> power, powerSm, floorP, rawGain, gainSm;
    bool primed = false; // powerSm has a first frame
    float confSm = 0.0f;

    Impl(int r, int ch)
        : rate(r)
        , channels(std::max(1, ch))
        , binHz(static_cast<float>(r) / kFft)
        , usedBins(std::min(kBins, static_cast<int>(kFadeToHz / (static_cast<float>(r) / kFft)) + 2))
        , minHalf(std::max(1, static_cast<int>(std::lround(70.0f / (static_cast<float>(r) / kFft)))))
    {
        in.assign(channels, {});
        acc.assign(channels, std::vector<float>(kFft, 0.0f));
        spec.assign(channels, std::vector<AVComplexFloat>(kBins));
        power.assign(kBins, 0.0f);
        powerSm.assign(kBins, 0.0f);
        floorP.assign(kBins, 0.0f);
        rawGain.assign(kBins, 1.0f);
        gainSm.assign(kBins, 1.0f);
    }

    void reset()
    {
        for (auto& c : in)
            c.clear();
        inBase = 0;
        inCount = 0;
        nextFrame = kFirstFrame;
        for (auto& a : acc)
            std::fill(a.begin(), a.end(), 0.0f);
        ready.clear();
        readyPos = 0;
        outCount = 0;
        std::fill(gainSm.begin(), gainSm.end(), 1.0f);
        primed = false;
        confSm = 0.0f;
    }

    float inputAt(const std::vector<float>& v, int64_t i) const
    {
        const int64_t k = i - inBase;
        return (k >= 0 && k < static_cast<int64_t>(v.size())) ? v[static_cast<size_t>(k)] : 0.0f;
    }

    // Weighted least-squares line through the floor, dB against octaves,
    // over kFitLoHz..kFitHiHz, each octave weighted alike. Returns the slope
    // (dB/octave) and sets `dbAt0` (the line's dB at log2(f) = 0).
    float fitFloor(float& dbAt0) const
    {
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        const int k0 = std::max(1, static_cast<int>(std::ceil(kFitLoHz / binHz)));
        const int k1 = std::min(usedBins - 1, static_cast<int>(kFitHiHz / binHz));
        for (int k = k0; k <= k1; ++k) {
            const double f = k * binHz;
            const double w = 1.0 / f;
            const double x = std::log2(f);
            const double y = 10.0 * std::log10(floorP[k] + 1e-20);
            sw += w;
            sx += w * x;
            sy += w * y;
            sxx += w * x * x;
            sxy += w * x * y;
        }
        const double d = sw * sxx - sx * sx;
        if (sw <= 0 || std::fabs(d) < 1e-12) {
            dbAt0 = -200.0f;
            return 0.0f;
        }
        const double slope = (sw * sxy - sx * sy) / d;
        dbAt0 = static_cast<float>((sy - slope * sx) / sw);
        return static_cast<float>(slope);
    }

    void processFrame(int64_t start, float strength)
    {
        std::fill(power.begin(), power.end(), 0.0f);
        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < kFft; ++i)
                tx.real[i] = inputAt(in[c], start + i) * win[i];
            tx.forward();
            std::copy(tx.cplx, tx.cplx + kBins, spec[c].begin());
            for (int k = 0; k < usedBins; ++k)
                power[k] += spec[c][k].re * spec[c][k].re + spec[c][k].im * spec[c][k].im;
        }

        const float frameSec = static_cast<float>(kHop) / rate;
        const float smA = std::exp(-frameSec / 0.03f); // power: ~30 ms
        const float confA = std::exp(-frameSec / 0.05f);
        const float atkA = std::exp(-frameSec / 0.01f); // gain: cut in ~10 ms
        const float relA = std::exp(-frameSec / 0.06f); // ... recover in ~60 ms
        for (int k = 0; k < usedBins; ++k) {
            const float p = power[k] / static_cast<float>(channels);
            powerSm[k] = primed ? smA * powerSm[k] + (1 - smA) * p : p;
        }
        primed = true;

        // The floor between tones.
        for (int k = 0; k < usedBins; ++k) {
            float m = powerSm[k];
            for (int j = std::max(0, k - minHalf); j <= std::min(usedBins - 1, k + minHalf); ++j)
                m = std::min(m, powerSm[j]);
            floorP[k] = m * kFloorBias;
        }

        float dbAt0 = 0.0f;
        const float slope = fitFloor(dbAt0);
        const float conf = std::clamp((-slope - 3.0f) / 4.0f, 0.0f, 1.0f);
        confSm = confA * confSm + (1 - confA) * conf;

        const float over = 2.0f * strength;
        const float minGain = std::pow(10.0f, -kMaxCutDb * strength / 20.0f);
        for (int k = 0; k < usedBins; ++k) {
            const float hz = k * binHz;
            float w = floorP[k];
            if (hz > kFitHiHz && hz > 0.0f)
                w = std::min(w, std::pow(10.0f, (dbAt0 + slope * std::log2(hz)) / 10.0f));
            w *= confSm * fade(hz);
            // Power subtraction; the gain on the amplitude is its root.
            const float g = 1.0f - over * w / (powerSm[k] + 1e-20f);
            rawGain[k] = std::clamp(std::sqrt(std::max(g, 0.0f)), minGain, 1.0f);
        }
        // Each bin gets the largest gain of itself and its neighbours: a
        // tone spreads over ~3 bins, and cutting its sides cuts the tone.
        // Then smoothed over time.
        for (int k = 0; k < usedBins; ++k) {
            const float l = rawGain[std::max(0, k - 1)];
            const float r = rawGain[std::min(usedBins - 1, k + 1)];
            const float g = std::max({l, rawGain[k], r});
            const float a = g < gainSm[k] ? atkA : relA;
            gainSm[k] = a * gainSm[k] + (1 - a) * g;
        }

        for (int c = 0; c < channels; ++c) {
            for (int k = 0; k < kBins; ++k) {
                const float g = k < usedBins ? gainSm[k] : 1.0f;
                tx.cplx[k].re = spec[c][k].re * g;
                tx.cplx[k].im = spec[c][k].im * g;
            }
            tx.inverse();
            float* a = acc[c].data(); // sqrt-Hann twice at 75 % overlap sums to 2
            for (int i = 0; i < kFft; ++i)
                a[i] += tx.real[i] * win[i] * 0.5f;
        }

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
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < channels; ++c)
                in[c].push_back(pcm[static_cast<size_t>(i) * channels + c]);
        inCount += frames;
    }

    void run(float strength)
    {
        while (inCount >= nextFrame + kLatency) {
            processFrame(nextFrame, strength);
            nextFrame += kHop;
        }
        const int64_t drop = nextFrame - inBase; // nothing reads before nextFrame
        if (drop > 4 * kFft) {
            for (auto& c : in)
                c.erase(c.begin(), c.begin() + drop);
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

GeneralWindReducer::GeneralWindReducer(int sampleRate, int channels)
    : m_impl(std::make_unique<Impl>(sampleRate, channels))
{
}

GeneralWindReducer::~GeneralWindReducer() = default;

void GeneralWindReducer::setStrength(float strength)
{
    m_strength = std::clamp(strength, 0.0f, 1.0f);
}

void GeneralWindReducer::reset()
{
    m_impl->reset();
}

int GeneralWindReducer::latency() const
{
    return kLatency;
}

float GeneralWindReducer::lastWindConfidence() const
{
    return m_impl->confSm;
}

void GeneralWindReducer::process(const float* in, float* out, int frames)
{
    if (!m_impl->tx.ok()) { // no FFT (never seen): silence
        std::fill(out, out + static_cast<size_t>(frames) * m_impl->channels, 0.0f);
        return;
    }
    m_impl->push(in, frames);
    m_impl->run(m_strength);
    m_impl->pop(out, frames);
}

} // namespace insv
