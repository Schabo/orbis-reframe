// SPDX-License-Identifier: MIT OR Apache-2.0
#include "HissHumReducer.h"
#include "Rdft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace insv {

namespace {

// --- hiss: the STFT stage (framing as in GeneralWindReducer) ---
constexpr int kFft = 2048;     // 23 Hz bins, ~43 ms at 48 kHz
constexpr int kHop = kFft / 4; // sqrt-Hann in and out, 75 % overlap
constexpr int kBins = kFft / 2 + 1;
constexpr int64_t kFirstFrame = -(kFft - kHop);
constexpr int kLatency = kFft;

constexpr float kMaxCutDb = 24.0f;   // at strength 1
constexpr float kMinWindowSec = 1.5f; // the noise estimate: quietest over this long
constexpr int kSubWindows = 8;        // ... kept as this many sub-window minima
constexpr float kDdAlpha = 0.98f;     // decision-directed SNR smoothing
// The estimate is a minimum over ~7 bins and ~1.5 s of a noisy power
// estimate, which sits well below the noise's mean; this brings it back up
// (x6.9 measured on steady white noise at 48 kHz).
constexpr float kNoiseBias = 7.0f;

// --- hum: detection + notches ---
constexpr int kHumFft = 32768; // 1.46 Hz bins, 0.68 s at 48 kHz
constexpr int kHumHop = 4096;  // a detection every ~85 ms
constexpr int kHumMaxHarmonics = 20;
constexpr float kHumMaxHz = 1000.0f;
constexpr float kHumSearchHz = 0.3f;  // mains frequency tolerance
constexpr float kHumSteadyHz = 0.1f; // max wander over the last second
constexpr int kHumSteadyN = 12;       // detections in that second
constexpr float kHumMaxDepthDb = 40.0f; // at strength 1

} // namespace

struct HissHumReducer::Impl {
    const int rate;
    const int channels;
    const int minHalf; // floor: minimum over +-minHalf bins (~70 Hz)
    const int subLen;  // frames per sub-window of the noise minimum

    // --- hiss ---
    Rdft tx{kFft, true};
    const std::vector<float> win = sqrtHann(kFft);

    // Input (after the hum notches) per channel from absolute frame
    // `inBase` on; `inCount` frames pushed in total.
    std::vector<std::vector<float>> in;
    int64_t inBase = 0;
    int64_t inCount = 0;
    int64_t nextFrame = kFirstFrame;

    std::vector<std::vector<float>> acc; // overlap-add, per channel
    std::vector<float> ready;            // finished output, interleaved
    size_t readyPos = 0;
    int64_t outCount = 0;

    std::vector<std::vector<AVComplexFloat>> spec;
    std::vector<float> power, powerSm, floorP, noise, cleanPrev, rawGain, gain;
    std::vector<float> subMin;                // this sub-window's minimum
    std::vector<std::vector<float>> subMins;  // the last kSubWindows' minima
    int subFill = 0;                          // frames in subMin
    int subCount = 0;                         // valid entries in subMins
    int subNext = 0;                          // ring position in subMins
    bool primed = false;                      // powerSm has a first frame
    // After rewind(): the noise estimate and the hum are held until the
    // input passes this frame (what the first pass saw); the next frame's
    // SNR starts without history.
    int64_t holdUntil = 0;
    bool ddFresh = false;

    // --- hum ---
    Rdft humTx{kHumFft, false};
    std::vector<float> humWin;
    std::vector<float> humPow;
    std::vector<float> humRing; // mono input, the last kHumFft frames
    int64_t humCount = 0;       // frames pushed into humRing
    int humBase = 0;            // 50 or 60 once a hum was found; 0 before
    float humF = 0.0f;          // its fundamental (smoothed), 0 = none
    float humConf = 0.0f;       // smoothed
    // The last kHumSteadyN detections' best fundamentals (0 = none).
    std::array<float, kHumSteadyN> humHist{};
    int humHistPos = 0;
    int humLastBase = 0;
    std::array<float, kHumMaxHarmonics> depthSm{}; // per harmonic, 0..1
    std::array<float, kHumMaxHarmonics> depth{};   // applied: x strength
    struct Coef {
        float b0 = 0, a1 = 0, a2 = 0; // band-pass, b1 = 0, b2 = -b0
    };
    std::array<Coef, kHumMaxHarmonics> coef{};
    int humHarmonics = 0;                                 // notches running
    std::vector<std::array<float, 2 * kHumMaxHarmonics>> z; // per channel
    std::vector<float> frameBuf; // one frame on its way through the notches

    Impl(int r, int ch)
        : rate(r)
        , channels(std::max(1, ch))
        , minHalf(std::max(1, static_cast<int>(std::lround(70.0f / (static_cast<float>(r) / kFft)))))
        , subLen(std::max(1, static_cast<int>(std::lround(kMinWindowSec * r / kHop / kSubWindows))))
    {
        in.assign(channels, {});
        acc.assign(channels, std::vector<float>(kFft, 0.0f));
        spec.assign(channels, std::vector<AVComplexFloat>(kBins));
        for (auto* v : {&power, &powerSm, &floorP, &noise, &cleanPrev, &rawGain, &gain})
            v->assign(kBins, 0.0f);
        subMin.assign(kBins, std::numeric_limits<float>::max());
        subMins.assign(kSubWindows, std::vector<float>(kBins, 0.0f));

        humWin.resize(kHumFft);
        for (int i = 0; i < kHumFft; ++i)
            humWin[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kHumFft));
        humPow.assign(kHumFft / 2 + 1, 0.0f);
        humRing.assign(kHumFft, 0.0f);
        z.assign(channels, {});
        frameBuf.assign(channels, 0.0f);
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
        std::fill(cleanPrev.begin(), cleanPrev.end(), 0.0f);
        std::fill(subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
        subFill = subCount = subNext = 0;
        primed = false;
        holdUntil = 0;
        ddFresh = false;

        std::fill(humRing.begin(), humRing.end(), 0.0f);
        humCount = 0;
        humBase = 0;
        humF = humConf = 0.0f;
        humHist.fill(0.0f);
        humHistPos = 0;
        humLastBase = 0;
        depthSm.fill(0.0f);
        depth.fill(0.0f);
        humHarmonics = 0;
        for (auto& s : z)
            s.fill(0.0f);
    }

    // Back to input frame 0, keeping the noise minima and the hum found.
    void rewind()
    {
        holdUntil = inCount;
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
        primed = false;
        ddFresh = true;
        std::fill(humRing.begin(), humRing.end(), 0.0f);
        humCount = 0;
        for (auto& s : z)
            s.fill(0.0f);
    }

    // --- hum ---

    float humBinHz() const { return static_cast<float>(rate) / kHumFft; }

    // Power at `hz`, linear between bins.
    float humPowAt(float hz) const
    {
        const float b = hz / humBinHz();
        const int i = static_cast<int>(b);
        if (i < 0 || i + 1 >= static_cast<int>(humPow.size()))
            return 0.0f;
        const float t = b - static_cast<float>(i);
        return humPow[i] * (1 - t) + humPow[i + 1] * t;
    }

    // How far (dB) the peak at `hz` stands above the spectrum 4-15 Hz to
    // either side (its median: other tones there don't lift it).
    float prominence(float hz) const
    {
        const float bh = humBinHz();
        const int c = static_cast<int>(hz / bh);
        if (c < 1 || c + 2 >= static_cast<int>(humPow.size()))
            return 0.0f;
        const float peak = std::max(humPow[c], humPow[c + 1]);
        float bg[64];
        int n = 0;
        const int lo = static_cast<int>(std::ceil(4.0f / bh));
        const int hi = std::min(31, static_cast<int>(15.0f / bh));
        for (int d = lo; d <= hi; ++d) {
            if (c - d >= 0)
                bg[n++] = humPow[c - d];
            if (c + 1 + d < static_cast<int>(humPow.size()))
                bg[n++] = humPow[c + 1 + d];
        }
        if (n == 0)
            return 0.0f;
        std::nth_element(bg, bg + n / 2, bg + n);
        return 10.0f * std::log10((peak + 1e-30f) / (bg[n / 2] + 1e-30f));
    }

    // The n-th largest of `v` (1-based): "at least n of these stand out".
    template <size_t N>
    static float nthLargest(std::array<float, N> v, int n)
    {
        std::nth_element(v.begin(), v.begin() + (n - 1), v.end(), std::greater<float>());
        return v[static_cast<size_t>(n - 1)];
    }

    // The spectrum's peak near `hz` (+-2 bins), to a fraction of a bin: a
    // parabola through the log power of the top bin and its neighbours.
    float peakHz(float hz) const
    {
        const float bh = humBinHz();
        const int c = static_cast<int>(std::lround(hz / bh));
        if (c < 3 || c + 3 >= static_cast<int>(humPow.size()))
            return hz;
        int m = c;
        for (int i = c - 2; i <= c + 2; ++i)
            if (humPow[i] > humPow[m])
                m = i;
        const float a = std::log(humPow[m - 1] + 1e-30f), b = std::log(humPow[m] + 1e-30f),
                    d = std::log(humPow[m + 1] + 1e-30f);
        const float den = a - 2 * b + d;
        const float off = den < 0 ? std::clamp(0.5f * (a - d) / den, -0.5f, 0.5f) : 0.0f;
        return (m + off) * bh;
    }

    struct Candidate {
        float f = 0.0f;    // fundamental
        float conf = 0.0f; // 0..1, before the steadiness test
        std::array<float, kHumMaxHarmonics> prom{};
        int harmonics = 0;
    };

    Candidate evaluate(float base) const
    {
        Candidate c;
        float best = -1.0f;
        for (float f = base - kHumSearchHz; f <= base + kHumSearchHz + 1e-4f; f += 0.02f) {
            float s = 0.0f;
            for (int k = 1; k <= 6; ++k)
                s += humPowAt(k * f);
            if (s > best) {
                best = s;
                c.f = f;
            }
        }
        // Refine from the harmonics that stand out, the higher ones weighted
        // up (an error in the fundamental grows with k up the series).
        double sw = 0.0, sf = 0.0;
        for (int k = 1; k * c.f < kHumMaxHz && k <= kHumMaxHarmonics; ++k) {
            if (prominence(k * c.f) < 10.0f)
                continue;
            const float fk = peakHz(k * c.f) / k;
            if (std::abs(fk - c.f) < 0.35f) {
                sw += k;
                sf += k * fk;
            }
        }
        if (sw > 0.0)
            c.f = static_cast<float>(sf / sw);
        if (std::abs(c.f - base) > kHumSearchHz)
            return c; // not mains (conf 0): an engine's harmonic near it

        c.harmonics = std::min(kHumMaxHarmonics, static_cast<int>(kHumMaxHz / c.f));
        c.harmonics = std::min(c.harmonics, static_cast<int>(0.45f * rate / c.f));
        std::array<float, 6> first{}, half{};
        std::array<float, 12> third{};
        for (int k = 1; k <= c.harmonics; ++k)
            c.prom[k - 1] = prominence(k * c.f);
        for (int k = 1; k <= 6; ++k) {
            first[k - 1] = k <= c.harmonics ? c.prom[k - 1] : 0.0f;
            half[k - 1] = prominence((k - 0.5f) * c.f);
            third[2 * k - 2] = prominence((k - 1.0f / 3) * c.f);
            third[2 * k - 1] = prominence((k - 2.0f / 3) * c.f);
        }
        // Hum: at least 3 of the first 6 harmonics stand out, by 10-16 dB
        // or more (in 1.46 Hz bins, so a hum even that far out is quiet
        // next to broadband noise; below 10, a strong tone's window skirt
        // can fake one). An engine
        // whose firing rate is half or a third of the mains' has those too,
        // but also lines at most of the points in between (a stray tone,
        // e.g. a voice's harmonic, lands on one or two at most).
        const float m = nthLargest(first, 3);
        const float between = std::max(nthLargest(half, 4), nthLargest(third, 8));
        c.conf = std::clamp((m - 10.0f) / 6.0f, 0.0f, 1.0f) *
                 std::clamp(1.0f - (between - 4.0f) / 6.0f, 0.0f, 1.0f);
        return c;
    }

    void detectHum(float strength)
    {
        const int pos = static_cast<int>(humCount % kHumFft); // oldest sample
        for (int i = 0; i < kHumFft; ++i)
            humTx.real[i] = humRing[(pos + i) % kHumFft] * humWin[i];
        humTx.forward();
        for (size_t k = 0; k < humPow.size(); ++k)
            humPow[k] = humTx.cplx[k].re * humTx.cplx[k].re + humTx.cplx[k].im * humTx.cplx[k].im;

        const Candidate c50 = evaluate(50.0f), c60 = evaluate(60.0f);
        const Candidate* pick = c60.conf > c50.conf ? &c60 : &c50;
        const int pickBase = pick == &c60 ? 60 : 50;
        if (humBase != 0 && pickBase != humBase && humConf > 0.05f)
            pick = humBase == 60 ? &c60 : &c50; // keep the hum we have until it fades
        const int base = pick == &c60 ? 60 : 50;
        // Mains holds its frequency; an engine's drifts with the revs, even
        // cruising: steady = within kHumSteadyHz over the last second.
        if (base != humLastBase)
            humHist.fill(0.0f);
        humLastBase = base;
        humHist[humHistPos] = pick->conf > 0.0f ? pick->f : 0.0f;
        humHistPos = (humHistPos + 1) % kHumSteadyN;
        const auto [lo, hi] = std::minmax_element(humHist.begin(), humHist.end());
        const bool steady = *lo > 0.0f && *hi - *lo < kHumSteadyHz;
        const float target = steady ? pick->conf : 0.0f;

        const float dt = static_cast<float>(kHumHop) / rate;
        const float rise = std::exp(-dt / 0.3f), fall = std::exp(-dt / 1.0f);
        if (base != humBase && humConf <= 0.05f && target > 0.0f) {
            humBase = base; // a new hum (or the first): start the notches afresh
            humF = pick->f;
            depthSm.fill(0.0f);
            for (auto& s : z)
                s.fill(0.0f);
        }
        if (base == humBase) {
            const float a = target > humConf ? rise : fall;
            humConf = a * humConf + (1 - a) * target;
            if (target > 0.5f)
                humF += 0.3f * (pick->f - humF);
        } else {
            humConf *= fall;
        }
        if (humBase == 0)
            return;

        const int harmonics = std::min(kHumMaxHarmonics, static_cast<int>(kHumMaxHz / humF));
        humHarmonics = std::min(harmonics, static_cast<int>(0.45f * rate / humF));
        const float maxDepth = 1.0f - std::pow(10.0f, -kHumMaxDepthDb * strength / 20.0f);
        for (int k = 0; k < kHumMaxHarmonics; ++k) {
            const bool own = base == humBase && k < pick->harmonics;
            const float t = own ? humConf * std::clamp((pick->prom[k] - 4.0f) / 8.0f, 0.0f, 1.0f) : 0.0f;
            const float a = t > depthSm[k] ? rise : fall;
            depthSm[k] = a * depthSm[k] + (1 - a) * t;
            depth[k] = k < humHarmonics ? depthSm[k] * maxDepth : 0.0f;
            if (k >= humHarmonics)
                continue;
            // RBJ band-pass, 0 dB at the centre, ~2 Hz wide (a little wider
            // up the series, where the mains' own drift is multiplied).
            const float f = (k + 1) * humF;
            const double w0 = 2.0 * std::numbers::pi * f / rate;
            const double bw = 1.5 + 0.05 * (k + 1);
            const double alpha = std::sin(w0) / (2.0 * (f / bw));
            const double a0 = 1.0 + alpha;
            coef[k].b0 = static_cast<float>(alpha / a0);
            coef[k].a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
            coef[k].a2 = static_cast<float>((1.0 - alpha) / a0);
        }
    }

    // One frame, all channels, through the notches in place.
    void notch(float* x)
    {
        for (int c = 0; c < channels; ++c) {
            float v = x[c];
            float* s = z[c].data();
            for (int k = 0; k < humHarmonics; ++k) {
                const Coef& q = coef[k];
                const float bp = q.b0 * v + s[2 * k];
                s[2 * k] = -q.a1 * bp + s[2 * k + 1];
                s[2 * k + 1] = -q.b0 * v - q.a2 * bp;
                v -= depth[k] * bp;
            }
            x[c] = v;
        }
    }

    // --- hiss ---

    float inputAt(const std::vector<float>& v, int64_t i) const
    {
        const int64_t k = i - inBase;
        return (k >= 0 && k < static_cast<int64_t>(v.size())) ? v[static_cast<size_t>(k)] : 0.0f;
    }

    void processFrame(int64_t start, float strength)
    {
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

        const float frameSec = static_cast<float>(kHop) / rate;
        const float smA = std::exp(-frameSec / 0.03f); // power: ~30 ms
        for (int k = 0; k < kBins; ++k)
            powerSm[k] = primed ? smA * powerSm[k] + (1 - smA) * power[k] : power[k];

        // The floor between tones, then its quietest over ~1.5 s. Not from
        // the DC bin: audio is high-passed, so it holds next to nothing and
        // would pull the lowest bins' floor to zero.
        for (int k = 0; k < kBins; ++k) {
            float m = k > 0 ? powerSm[k] : powerSm[1];
            for (int j = std::max(1, k - minHalf); j <= std::min(kBins - 1, k + minHalf); ++j)
                m = std::min(m, powerSm[j]);
            floorP[k] = m;
        }
        // Frames that reach back before the first input are part silence:
        // they'd hold the minimum near zero for its whole window. Nor while
        // held after rewind().
        const bool learn = start >= 0 && start + kFft > holdUntil;
        if (learn)
            for (int k = 0; k < kBins; ++k)
                subMin[k] = std::min(subMin[k], floorP[k]);
        // No estimate yet (only part-silence frames so far): leave it all.
        const bool estimate = learn || subCount > 0 || subFill > 0;
        for (int k = 0; k < kBins && estimate; ++k) {
            float m = subMin[k];
            for (int s = 0; s < subCount; ++s)
                m = std::min(m, subMins[s][k]);
            noise[k] = m * kNoiseBias;
        }
        if (learn && ++subFill == subLen) {
            subMins[subNext] = subMin;
            subNext = (subNext + 1) % kSubWindows;
            subCount = std::min(subCount + 1, kSubWindows);
            subFill = 0;
            std::fill(subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
        }

        // Wiener gain from the decision-directed a-priori SNR.
        const float minGain = std::pow(10.0f, -kMaxCutDb * strength / 20.0f);
        for (int k = 0; k < kBins && !estimate; ++k) {
            cleanPrev[k] = power[k];
            rawGain[k] = 1.0f;
        }
        const float dd = ddFresh ? 0.0f : kDdAlpha;
        if (estimate)
            ddFresh = false;
        for (int k = 0; k < kBins && estimate; ++k) {
            const float n = noise[k] + 1e-20f;
            const float post = power[k] / n;
            const float prio = dd * cleanPrev[k] / n + (1 - dd) * std::max(post - 1.0f, 0.0f);
            const float g = prio / (1.0f + prio);
            cleanPrev[k] = g * g * power[k];
            rawGain[k] = std::clamp(g, minGain, 1.0f);
        }
        primed = true;
        // Each bin gets the largest gain of itself and its neighbours: a
        // tone spreads over ~3 bins, and cutting its sides cuts the tone.
        for (int k = 0; k < kBins; ++k)
            gain[k] = std::max({rawGain[std::max(0, k - 1)], rawGain[k], rawGain[std::min(kBins - 1, k + 1)]});

        for (int c = 0; c < channels; ++c) {
            for (int k = 0; k < kBins; ++k) {
                tx.cplx[k].re = spec[c][k].re * gain[k];
                tx.cplx[k].im = spec[c][k].im * gain[k];
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

    // Detection runs at fixed input positions (every kHumHop frames), so the
    // output doesn't depend on the block sizes.
    void push(const float* pcm, int frames, float strength)
    {
        for (int i = 0; i < frames; ++i) {
            const float* x = pcm + static_cast<size_t>(i) * channels;
            float mono = 0.0f;
            for (int c = 0; c < channels; ++c)
                mono += x[c];
            humRing[humCount % kHumFft] = mono / static_cast<float>(channels);
            ++humCount;
            if (humCount % kHumHop == 0 && humCount >= kHumFft / 2 && humCount > holdUntil)
                detectHum(strength);
            std::copy_n(x, channels, frameBuf.data());
            if (humHarmonics > 0)
                notch(frameBuf.data());
            for (int c = 0; c < channels; ++c)
                in[c].push_back(frameBuf[c]);
        }
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

HissHumReducer::HissHumReducer(int sampleRate, int channels)
    : m_impl(std::make_unique<Impl>(sampleRate, channels))
{
}

HissHumReducer::~HissHumReducer() = default;

void HissHumReducer::setStrength(float strength)
{
    m_strength = std::clamp(strength, 0.0f, 1.0f);
}

void HissHumReducer::reset()
{
    m_impl->reset();
}

void HissHumReducer::rewind()
{
    m_impl->rewind();
}

int HissHumReducer::latency() const
{
    return kLatency;
}

float HissHumReducer::humHz() const
{
    return m_impl->humConf > 0.05f ? m_impl->humF : 0.0f;
}

float HissHumReducer::humConfidence() const
{
    return m_impl->humConf;
}

void HissHumReducer::process(const float* in, float* out, int frames)
{
    if (!m_impl->tx.ok() || !m_impl->humTx.ok()) { // no FFT (never seen): silence
        std::fill(out, out + static_cast<size_t>(frames) * m_impl->channels, 0.0f);
        return;
    }
    m_impl->push(in, frames, m_strength);
    m_impl->run(m_strength);
    m_impl->pop(out, frames);
}

} // namespace insv
