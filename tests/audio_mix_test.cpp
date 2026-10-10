// insv::AudioMixer (extra audio tracks): placement, gain, fades, chunked vs.
// one-shot reads, seeking, the limiter -- on WAV files written here (constant
// PCM, so every mixed sample is exactly predictable). Also that InsvAudioDemux
// reports a WAV's duration (what "Add audio file" relies on). The per-clip
// noise filters (insv::WindReducer, GeneralWindReducer, HissHumReducer) on
// synthetic signals, and ClipAudioReader's alignment with them on. Exit code is
// the failure count.

#include "AudioMixer.h"
#include "AudioPeaks.h"
#include "ClipAudioReader.h"
#include "InsvAudioDemux.h"
#include "GeneralWindReducer.h"
#include "HissHumReducer.h"
#ifdef INSV_HAVE_VOICE
#include "VoiceIsolator.h"
#include "DataPaths.h"
#endif
#include "WindReducer.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr int kRate = 48000;
constexpr int kCh = 2;

// A 16-bit stereo WAV at 48 kHz. `sample(i)` gives frame i's value (both
// channels).
template <typename F>
void writeWav(const std::string& path, int frames, F sample)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * kCh * 2;
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(kCh);
    u32(kRate);
    u32(kRate * kCh * 2);
    u16(kCh * 2);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    for (int i = 0; i < frames; ++i) {
        const int16_t s = static_cast<int16_t>(std::lround(sample(i) * 32768.0));
        for (int c = 0; c < kCh; ++c)
            f.write(reinterpret_cast<const char*>(&s), 2);
    }
}

bool near(float a, float b, float tol = 1e-4f) { return std::abs(a - b) <= tol; }

void testDemuxDuration(const std::string& dc)
{
    insv::InsvAudioDemux d;
    std::string err;
    CHECK(d.open(dc, kRate, kCh, err));
    CHECK(d.durationMs() == 1000);
}

void testPlacementGainFades(const std::string& dc)
{
    // DC 0.25 for 1 s. Clip: timeline frames [4800, 4800 + 24000), gain 0.5,
    // 480-frame fade in, 960-frame fade out.
    insv::AudioMixer mix(kRate, kCh);
    insv::MixClip c;
    c.path = dc;
    c.startFrame = 4800;
    c.lengthFrames = 24000;
    c.inFrame = 2400;
    c.gain = 0.5f;
    c.fadeInFrames = 480;
    c.fadeOutFrames = 960;
    mix.setClips({c});
    CHECK(!mix.empty());

    const int64_t total = 33600;
    std::vector<float> one(total * kCh, 0.0f);
    mix.mixInto(one.data(), 0, total);
    auto at = [&](int64_t f) { return one[f * kCh]; };
    CHECK(at(0) == 0.0f && at(4799) == 0.0f);          // before the clip
    CHECK(at(4800) == 0.0f);                            // fade-in starts at silence
    CHECK(near(at(4800 + 240), 0.125f * 0.5f));         // half-way up
    CHECK(near(at(4800 + 480), 0.125f));                // full level: 0.25 * 0.5
    CHECK(near(at(15000), 0.125f) && near(one[15000 * kCh + 1], 0.125f));
    CHECK(near(at(4800 + 24000 - 961), 0.125f));        // just before the fade-out
    CHECK(near(at(4800 + 24000 - 481), 0.0625f));       // half-way down
    CHECK(at(4800 + 24000 - 1) == 0.0f);                // last frame: silent
    CHECK(at(4800 + 24000) == 0.0f && at(total - 1) == 0.0f); // after the clip

    // Adding, not overwriting: the same mix over a 0.1 floor.
    insv::AudioMixer mix2(kRate, kCh);
    mix2.setClips({c});
    std::vector<float> floor(total * kCh, 0.1f);
    mix2.mixInto(floor.data(), 0, total);
    CHECK(near(floor[15000 * kCh], 0.225f) && near(floor[0], 0.1f));

    // Chunked in odd sizes = one shot.
    insv::AudioMixer mix3(kRate, kCh);
    mix3.setClips({c});
    std::vector<float> chunked(total * kCh, 0.0f);
    int64_t pos = 0;
    for (int64_t n = 1; pos < total; n = n * 3 % 1777 + 1) {
        const int64_t k = std::min<int64_t>(n, total - pos);
        mix3.mixInto(chunked.data() + pos * kCh, pos, k);
        pos += k;
    }
    bool same = true;
    for (size_t i = 0; i < one.size(); ++i)
        same = same && near(one[i], chunked[i], 1e-6f);
    CHECK(same);
}

void testSeekAndSource(const std::string& ramp)
{
    // A ramp: frame i of the source holds i / 32768 (exact in 16 bits). The
    // clip starts at timeline 1000 with in-point 500, so timeline frame t
    // plays source frame t - 500.
    insv::AudioMixer mix(kRate, kCh);
    insv::MixClip c;
    c.path = ramp;
    c.startFrame = 1000;
    c.lengthFrames = 20000;
    c.inFrame = 500;
    mix.setClips({c});
    std::vector<float> buf(100 * kCh);
    auto expect = [](int64_t t) { return static_cast<float>(t - 500) / 32768.0f; };

    std::fill(buf.begin(), buf.end(), 0.0f);
    mix.mixInto(buf.data(), 3000, 100);
    CHECK(near(buf[0], expect(3000), 1e-5f) && near(buf[99 * kCh], expect(3099), 1e-5f));
    // Jump back (a seek): re-positions.
    std::fill(buf.begin(), buf.end(), 0.0f);
    mix.mixInto(buf.data(), 1200, 100);
    CHECK(near(buf[0], expect(1200), 1e-5f));
    // Jump past the clip, then back into it: the reader is reopened.
    std::fill(buf.begin(), buf.end(), 0.0f);
    mix.mixInto(buf.data(), 30000, 100);
    CHECK(buf[0] == 0.0f);
    mix.mixInto(buf.data(), 5000, 100);
    CHECK(near(buf[0], expect(5000), 1e-5f));
}

void testMissingAndMuted(const std::string& dc)
{
    insv::AudioMixer mix(kRate, kCh);
    insv::MixClip gone;
    gone.path = "/nonexistent/file.wav";
    gone.lengthFrames = 1000;
    insv::MixClip silent;
    silent.path = dc;
    silent.lengthFrames = 1000;
    silent.gain = 0.0f; // muted clips are dropped
    mix.setClips({gone, silent});
    std::vector<float> buf(1000 * kCh, 0.5f);
    mix.mixInto(buf.data(), 0, 1000);
    CHECK(buf[0] == 0.5f && buf[1999] == 0.5f); // a missing file adds silence
}

void testLimiter()
{
    float s[] = {0.0f, 0.5f, -0.8f, 0.89f, 0.95f, -1.5f, 3.0f, 100.0f};
    insv::AudioMixer::limit(s, 8);
    CHECK(s[0] == 0.0f && s[1] == 0.5f && s[2] == -0.8f && s[3] == 0.89f); // untouched
    CHECK(s[4] > 0.9f && s[4] < 0.95f);
    CHECK(s[5] < -0.95f && s[5] > -1.0f);
    CHECK(s[6] > s[4] && s[6] <= 1.0f && s[7] >= s[6] && s[7] <= 1.0f);
}

void testPeaks(const std::string& burst)
{
    // 1 s: silence, then a 0.5 sine from 0.4 s to 0.6 s, then DC 0.1.
    const insv::AudioPeaks p = insv::ComputeAudioPeaks(burst, -1);
    CHECK(p.peaks.size() >= 99 && p.peaks.size() <= 101);
    if (p.peaks.size() < 99)
        return;
    CHECK(p.peaks[10] < 0.01f && p.peaks[38] < 0.01f);
    CHECK(near(p.peaks[45], 0.5f, 0.02f) && near(p.peaks[55], 0.5f, 0.02f));
    CHECK(near(p.peaks[80], 0.1f, 0.02f));
    CHECK(insv::ComputeAudioPeaks("/nonexistent.wav", -1).peaks.empty());
    std::atomic<bool> stop{true};
    CHECK(insv::ComputeAudioPeaks(burst, -1, &stop).peaks.empty()); // cancelled up front
}


// --- WindReducer -----------------------------------------------------------

std::vector<float> interleave(const std::vector<float>& mono)
{
    std::vector<float> v(mono.size() * kCh);
    for (size_t i = 0; i < mono.size(); ++i)
        for (int c = 0; c < kCh; ++c)
            v[i * kCh + c] = mono[i];
    return v;
}

template <typename Filter = insv::WindReducer>
std::vector<float> runWind(const std::vector<float>& pcm, float strength, int block)
{
    Filter w(kRate, kCh);
    w.setStrength(strength);
    std::vector<float> out(pcm.size());
    const int frames = static_cast<int>(pcm.size() / kCh);
    for (int i = 0; i < frames; i += block) {
        const int n = std::min(block, frames - i);
        w.process(pcm.data() + i * kCh, out.data() + i * kCh, n);
    }
    return out;
}

// Largest |out(t) - in(t - latency)| over frames [from, end).
float delayedError(const std::vector<float>& in, const std::vector<float>& out, int from,
                   int lat = insv::WindReducer(kRate, kCh).latency())
{
    float err = 0.0f;
    for (size_t i = static_cast<size_t>(from) * kCh; i < out.size(); ++i)
        err = std::max(err, std::abs(out[i] - in[i - static_cast<size_t>(lat) * kCh]));
    return err;
}

std::vector<float> lowNoise(int frames, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, 0.1f);
    std::vector<float> v(static_cast<size_t>(frames));
    float lp = 0.0f;
    for (float& x : v) {
        lp += 0.05f * (n(rng) - lp); // one-pole ~400 Hz: wind-like rumble
        x = lp;
    }
    return v;
}

// Power at `hz` over mono frames [from, from + len) of interleaved `pcm`.
double goertzel(const std::vector<float>& pcm, int from, int len, double hz)
{
    const double w = 2.0 * std::numbers::pi * hz / kRate, cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) {
        const double s0 = pcm[static_cast<size_t>(from + i) * kCh] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - cw * s1 * s2;
}

void testWindLatencyAndPassBand()
{
    // 5 kHz is above the wind band: comes out unchanged, exactly latency()
    // frames late; the first latency() frames are silence.
    std::vector<float> sine(2 * kRate);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * static_cast<float>(std::sin(i * 2.0 * std::numbers::pi * 5000.0 / kRate));
    const auto in = interleave(sine);
    const auto out = runWind(in, 0.7f, 1000);
    const int lat = insv::WindReducer(kRate, kCh).latency();
    CHECK(lat > 0 && lat < kRate / 2);
    CHECK(out[0] == 0.0f && out[static_cast<size_t>(lat - 1) * kCh] == 0.0f);
    CHECK(delayedError(in, out, lat + 8192) < 1e-3f);
}

void testWindBlockSizes()
{
    // Any block size gives the same output as one call.
    const auto in = interleave(lowNoise(kRate, 1));
    const auto one = runWind(in, 0.7f, kRate);
    insv::WindReducer w(kRate, kCh);
    std::vector<float> out(in.size());
    std::mt19937 rng(7);
    for (int i = 0; i < kRate;) {
        const int n = std::min(1 + static_cast<int>(rng() % 3000), kRate - i);
        w.process(in.data() + i * kCh, out.data() + i * kCh, n);
        i += n;
    }
    float diff = 0.0f;
    for (size_t i = 0; i < out.size(); ++i)
        diff = std::max(diff, std::abs(out[i] - one[i]));
    CHECK(diff < 1e-6f);
}

void testWindStrengthZero()
{
    // Strength 0 leaves even the wind band alone.
    const auto in = interleave(lowNoise(kRate, 2));
    CHECK(delayedError(in, runWind(in, 0.0f, 4096), 20000) < 1e-3f);
}

void testWindKeepsEngineCutsWind()
{
    // Rumble alone drops by most of the strength (0.7 -> gain 0.3, -10.5
    // dB): a little less, since the tracker now and then takes a random comb
    // in the noise for a weak engine and protects it -- tuned so, because
    // the opposite mistake would cut a real engine.
    constexpr double f0 = 25.0;
    const int lat = insv::WindReducer(kRate, kCh).latency();
    const auto rumble = interleave(lowNoise(3 * kRate, 3));
    const auto rumbleOut = runWind(rumble, 0.7f, 2048);
    double rin = 0, rout = 0;
    for (double hz = 60.0; hz < 700.0; hz += 7.0) {
        rin += goertzel(rumble, kRate, kRate, hz);
        rout += goertzel(rumbleOut, kRate + lat, kRate, hz);
    }
    // An engine-like comb (25 Hz fundamental, harmonics to 1 kHz) over the
    // same rumble: the harmonics keep their level, the rumble between them
    // still drops. (Less than alone: at 5.9 Hz bins the harmonics' own
    // spread leaves little room between them -- as in the spike.)
    std::vector<float> sig = lowNoise(3 * kRate, 3);
    for (size_t i = 0; i < sig.size(); ++i)
        for (int h = 1; h * f0 < 1000.0; ++h)
            sig[i] += 0.02f * static_cast<float>(std::sin(i * 2.0 * std::numbers::pi * h * f0 / kRate + h));
    const auto in = interleave(sig);
    const auto out = runWind(in, 0.7f, 2048);
    double hin = 0, hout = 0, nin = 0, nout = 0;
    for (int h = 2; h < 30; ++h) {
        hin += goertzel(in, kRate, kRate, h * f0);
        hout += goertzel(out, kRate + lat, kRate, h * f0);
        nin += goertzel(in, kRate, kRate, (h + 0.5) * f0);
        nout += goertzel(out, kRate + lat, kRate, (h + 0.5) * f0);
    }
    const double rumbleDb = 10.0 * std::log10(rout / rin);
    const double harmDb = 10.0 * std::log10(hout / hin);
    const double betweenDb = 10.0 * std::log10(nout / nin);
    std::printf("wind: rumble alone %+.1f dB; with engine: harmonics %+.1f dB, between %+.1f dB\n",
                rumbleDb, harmDb, betweenDb);
    CHECK(rumbleDb < -6.0);
    CHECK(harmDb > -2.0);
    CHECK(betweenDb < harmDb - 1.5);
}

// --- GeneralWindReducer ----------------------------------------------------

// Gusty wind: white noise through two one-pole low-passes (~150 Hz, so
// -12 dB/octave above), its level wandering 0.3-1.5x a few times a second.
std::vector<float> windNoise(int frames, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, 1.0f);
    std::vector<float> v(static_cast<size_t>(frames));
    float a = 0.0f, b = 0.0f, gust = 1.0f, gustTarget = 1.0f;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i % 4800 == 0)
            gustTarget = 0.3f + 1.2f * static_cast<float>(rng() % 1000) / 1000.0f;
        gust += 0.0005f * (gustTarget - gust);
        a += 0.02f * (n(rng) - a);
        b += 0.02f * (a - b);
        v[i] = 0.5f * gust * b;
    }
    return v;
}

// A voice-like tone: 140 Hz harmonics to 3.5 kHz, 4 Hz syllable swell.
constexpr double kVoiceF0 = 140.0;
std::vector<float> voiceLike(int frames)
{
    std::vector<float> v(static_cast<size_t>(frames));
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double env = 0.65 + 0.35 * std::sin(2.0 * std::numbers::pi * 4.0 * t);
        double x = 0.0;
        for (int h = 1; h * kVoiceF0 < 3500.0; ++h)
            x += 0.03 / std::pow(h, 0.7) * std::sin(2.0 * std::numbers::pi * h * kVoiceF0 * t + h);
        v[i] = static_cast<float>(env * x);
    }
    return v;
}

// Summed power (dB change, out vs in) over `hzs`, one second from 1 s in.
double bandChangeDb(const std::vector<float>& in, const std::vector<float>& out, int lat,
                    const std::vector<double>& hzs)
{
    double pin = 0, pout = 0;
    for (double hz : hzs) {
        pin += goertzel(in, kRate, kRate, hz);
        pout += goertzel(out, kRate + lat, kRate, hz);
    }
    return 10.0 * std::log10(pout / pin);
}

void testGeneralWind()
{
    using GW = insv::GeneralWindReducer;
    const int lat = GW(kRate, kCh).latency();
    CHECK(lat > 0 && lat < kRate / 10); // short: fine for speech sync

    std::vector<double> low; // the wind band, off the voice's harmonics
    for (double hz = 50.0; hz < 700.0; hz += 11.0)
        if (std::fmod(hz, kVoiceF0) > 25.0 && std::fmod(hz, kVoiceF0) < kVoiceF0 - 25.0)
            low.push_back(hz);
    std::vector<double> harm;
    for (int h = 1; h * kVoiceF0 < 3500.0; ++h)
        harm.push_back(h * kVoiceF0);
    std::vector<double> wide;
    for (double hz = 50.0; hz < 4000.0; hz += 13.0)
        wide.push_back(hz);

    // Wind alone: well down.
    const auto wind = interleave(windNoise(3 * kRate, 11));
    const double windDb = bandChangeDb(wind, runWind<GW>(wind, 0.7f, 2048), lat, low);

    // A voice alone, and hiss alone: not wind -- left alone.
    const auto voice = interleave(voiceLike(3 * kRate));
    const double voiceDb = bandChangeDb(voice, runWind<GW>(voice, 0.7f, 2048), lat, harm);
    std::vector<float> hissMono(3 * kRate);
    std::mt19937 rng(5);
    std::normal_distribution<float> n(0.0f, 0.05f);
    for (float& x : hissMono)
        x = n(rng);
    const auto hiss = interleave(hissMono);
    const double hissDb = bandChangeDb(hiss, runWind<GW>(hiss, 0.7f, 2048), lat, wide);

    // The voice in the wind: harmonics kept, the wind between them cut.
    std::vector<float> mix = windNoise(3 * kRate, 11);
    const auto v = voiceLike(3 * kRate);
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] += v[i];
    const auto mixIn = interleave(mix);
    const auto mixOut = runWind<GW>(mixIn, 0.7f, 2048);
    const double mixHarmDb = bandChangeDb(mixIn, mixOut, lat, harm);
    const double mixLowDb = bandChangeDb(mixIn, mixOut, lat, low);

    std::printf("general wind: wind %+.1f dB, voice %+.1f dB, hiss %+.1f dB; "
                "voice in wind: harmonics %+.1f dB, between %+.1f dB\n",
                windDb, voiceDb, hissDb, mixHarmDb, mixLowDb);
    CHECK(windDb < -8.0);
    CHECK(std::abs(voiceDb) < 1.0);
    CHECK(std::abs(hissDb) < 1.0);
    CHECK(mixHarmDb > -3.0);
    CHECK(mixLowDb < -6.0);

    // 5 kHz passes exactly, latency() late; strength 0 changes nothing;
    // any block size gives the same output.
    std::vector<float> sine(2 * kRate);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * static_cast<float>(std::sin(i * 2.0 * std::numbers::pi * 5000.0 / kRate));
    const auto sin5k = interleave(sine);
    CHECK(delayedError(sin5k, runWind<GW>(sin5k, 0.7f, 1000), lat + 2048, lat) < 1e-3f);
    CHECK(delayedError(wind, runWind<GW>(wind, 0.0f, 4096), lat + 2048, lat) < 1e-3f);
    const auto one = runWind<GW>(wind, 0.7f, 3 * kRate);
    const auto odd = runWind<GW>(wind, 0.7f, 777);
    float diff = 0.0f;
    for (size_t i = 0; i < one.size(); ++i)
        diff = std::max(diff, std::abs(one[i] - odd[i]));
    CHECK(diff < 1e-6f);
}

// --- HissHumReducer --------------------------------------------------------

std::vector<float> whiteNoise(int frames, float sigma, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, sigma);
    std::vector<float> v(static_cast<size_t>(frames));
    for (float& x : v)
        x = n(rng);
    return v;
}

// A steady comb: `f0` and its harmonics up to `maxHz`, each `amp` / h.
void addComb(std::vector<float>& v, double f0, double maxHz, double amp)
{
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        double x = 0.0;
        for (int h = 1; h * f0 < maxHz; ++h)
            x += amp / h * std::sin(2.0 * std::numbers::pi * h * f0 * t + 0.3 * h);
        v[i] += static_cast<float>(x);
    }
}

// dB change over `hzs`, like bandChangeDb but over the 4th second (the
// noise estimate and the hum detection have settled by then).
double lateChangeDb(const std::vector<float>& in, const std::vector<float>& out, int lat,
                    const std::vector<double>& hzs)
{
    double pin = 0, pout = 0;
    for (double hz : hzs) {
        pin += goertzel(in, 3 * kRate, kRate, hz);
        pout += goertzel(out, 3 * kRate + lat, kRate, hz);
    }
    return 10.0 * std::log10(pout / pin);
}

void testHissHum()
{
    using HH = insv::HissHumReducer;
    const int lat = HH(kRate, kCh).latency();
    CHECK(lat > 0 && lat < kRate / 10);
    constexpr int kLen = 5 * kRate;

    std::vector<double> harm, between, wide;
    for (int h = 1; h * kVoiceF0 < 3500.0; ++h) {
        harm.push_back(h * kVoiceF0);
        between.push_back((h + 0.5) * kVoiceF0);
    }
    for (double hz = 100.0; hz < 15000.0; hz += 37.0)
        wide.push_back(hz);

    // Hiss alone: down by about the strength's cap (0.7 -> 16.8 dB).
    const auto hiss = interleave(whiteNoise(kLen, 0.02f, 21));
    const double hissDb = lateChangeDb(hiss, runWind<HH>(hiss, 0.7f, 2048), lat, wide);

    // A voice alone: kept.
    const auto voice = interleave(voiceLike(kLen));
    const double voiceDb = lateChangeDb(voice, runWind<HH>(voice, 0.7f, 2048), lat, harm);

    // A voice in hiss: harmonics kept, the hiss between them cut.
    std::vector<float> mix = whiteNoise(kLen, 0.01f, 22);
    const auto v = voiceLike(kLen);
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] += v[i];
    const auto mixIn = interleave(mix);
    const auto mixOut = runWind<HH>(mixIn, 0.7f, 2048);
    const double mixHarmDb = lateChangeDb(mixIn, mixOut, lat, harm);
    const double mixBetweenDb = lateChangeDb(mixIn, mixOut, lat, between);

    std::printf("hiss/hum: hiss %+.1f dB, voice %+.1f dB; voice in hiss: harmonics %+.1f dB, "
                "between %+.1f dB\n",
                hissDb, voiceDb, mixHarmDb, mixBetweenDb);
    CHECK(hissDb < -12.0);
    CHECK(std::abs(voiceDb) < 1.0);
    CHECK(mixHarmDb > -2.0);
    CHECK(mixBetweenDb < -8.0);

    // Mains hum (50.1 and 59.9 Hz, harmonics to 1 kHz) under a voice and a
    // little hiss: found, notched; the voice's harmonics stay.
    for (double mains : {50.1, 59.9}) {
        std::vector<float> sig = whiteNoise(kLen, 0.002f, 23);
        addComb(sig, mains, 1000.0, 0.05);
        const auto vv = voiceLike(kLen);
        for (size_t i = 0; i < sig.size(); ++i)
            sig[i] += vv[i];
        const auto in = interleave(sig);
        HH h(kRate, kCh);
        h.setStrength(0.7f);
        std::vector<float> out(in.size());
        h.process(in.data(), out.data(), kLen);
        std::vector<double> hum;
        for (int k = 1; k * mains < 1000.0; ++k)
            if (std::abs(std::remainder(k * mains, kVoiceF0)) > 15.0)
                hum.push_back(k * mains);
        const double humDb = lateChangeDb(in, out, lat, hum);
        const double keepDb = lateChangeDb(in, out, lat, harm);
        std::printf("hiss/hum: %.1f Hz hum found at %.2f Hz (conf %.2f): hum %+.1f dB, "
                    "voice %+.1f dB\n",
                    mains, h.humHz(), h.humConfidence(), humDb, keepDb);
        CHECK(std::abs(h.humHz() - mains) < 0.05);
        CHECK(h.humConfidence() > 0.8f);
        CHECK(humDb < -20.0);
        CHECK(keepDb > -2.0);
    }

    // An engine-like comb at 25 Hz (its harmonics include 50, 100, ...):
    // not taken for hum, so the harmonics on 50 Hz multiples fare no worse
    // than those between. (The hiss stage does turn the whole comb down: at
    // 23 Hz bins a steady comb this dense is indistinguishable from a
    // steady noise floor.)
    {
        std::vector<float> sig = whiteNoise(kLen, 0.002f, 24);
        addComb(sig, 25.0, 1000.0, 0.05);
        const auto in = interleave(sig);
        HH h(kRate, kCh);
        h.setStrength(0.7f);
        std::vector<float> out(in.size());
        h.process(in.data(), out.data(), kLen);
        std::vector<double> on50, off50;
        for (int k = 2; k * 25.0 < 1000.0; ++k)
            (k % 2 == 0 ? on50 : off50).push_back(k * 25.0);
        const double on50Db = lateChangeDb(in, out, lat, on50);
        const double off50Db = lateChangeDb(in, out, lat, off50);
        std::printf("hiss/hum: 25 Hz engine comb: hum conf %.2f, harmonics on 50 Hz multiples "
                    "%+.1f dB, between %+.1f dB\n",
                    h.humConfidence(), on50Db, off50Db);
        CHECK(h.humConfidence() < 0.2f);
        CHECK(std::abs(on50Db - off50Db) < 1.5);
    }

    // 5 kHz passes, latency() late -- all but the far skirt of its
    // spectrum (the noise estimate there is the skirt further out), ~-36 dB;
    // strength 0 changes nothing, hum or not; any block size gives the same
    // output.
    std::vector<float> sine(3 * kRate);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * static_cast<float>(std::sin(i * 2.0 * std::numbers::pi * 5000.0 / kRate));
    const auto sin5k = interleave(sine);
    CHECK(delayedError(sin5k, runWind<HH>(sin5k, 0.7f, 1000), lat + 2048, lat) < 0.015f);
    std::vector<float> humMono = whiteNoise(kLen, 0.01f, 25);
    addComb(humMono, 50.0, 1000.0, 0.05);
    const auto humIn = interleave(humMono);
    CHECK(delayedError(humIn, runWind<HH>(humIn, 0.0f, 4096), lat + 2048, lat) < 1e-3f);
    const auto one = runWind<HH>(humIn, 0.7f, kLen);
    const auto odd = runWind<HH>(humIn, 0.7f, 777);
    float diff = 0.0f;
    for (size_t i = 0; i < one.size(); ++i)
        diff = std::max(diff, std::abs(one[i] - odd[i]));
    CHECK(diff < 1e-6f);
}

// Hiss + 50 Hz hum from a file's first sample: through ClipAudioReader
// (which learns from the opening seconds, then rewinds) the first second is
// cleaned like the rest -- not left for the ~2 s the filter needs to settle.
std::vector<float> hissHumSignal(int frames)
{
    std::vector<float> v = whiteNoise(frames, 0.01f, 31);
    addComb(v, 50.0, 1000.0, 0.05);
    return v;
}

void testHissHumAtFileStart(const std::string& wav)
{
    const int frames = 4 * kRate;
    const auto src = hissHumSignal(frames); // as written, before 16-bit rounding
    insv::ClipAudioReader r(kRate, kCh);
    CHECK(r.open(wav, -1));
    r.setNoiseReduction({insv::NoiseMode::Hiss, 0.5f});
    r.seek(0);
    std::vector<float> out(static_cast<size_t>(frames) * kCh);
    r.read(out.data(), frames);
    const auto in = interleave(src);
    std::vector<double> hum, hiss;
    for (int k = 2; k <= 6; ++k)
        hum.push_back(k * 50.0);
    for (double hz = 1000.0; hz < 12000.0; hz += 97.0)
        hiss.push_back(hz);
    auto change = [&](int from, const std::vector<double>& hzs) {
        double pin = 0, pout = 0;
        for (double hz : hzs) {
            pin += goertzel(in, from, kRate / 2, hz);
            pout += goertzel(out, from, kRate / 2, hz); // the reader's output is aligned
        }
        return 10.0 * std::log10(pout / pin);
    };
    const double humStart = change(kRate / 10, hum), humLate = change(3 * kRate, hum);
    const double hissStart = change(kRate / 10, hiss), hissLate = change(3 * kRate, hiss);
    std::printf("hiss/hum from a file's start (0.1-0.6 s vs 3-3.5 s): hum %+.1f / %+.1f dB, "
                "hiss %+.1f / %+.1f dB\n",
                humStart, humLate, hissStart, hissLate);
    // The hum a little less at first: ~2 Hz wide notches take a few tenths
    // of a second to ring in.
    CHECK(humStart < -15.0 && humLate < -20.0);
    CHECK(hissStart < -6.0 && std::abs(hissStart - hissLate) < 2.0);
}

#ifdef INSV_HAVE_VOICE
// VoiceIsolator (DeepFilterNet): no speech to hand here (its output on real
// speech was checked against the reference deep-filter CLI, 2026-09-30),
// so what synthetic signals can show -- noise alone is taken
// out (Voice only) or turned down by the limit (Voice + ambience); strength
// 0 changes nothing; the output doesn't depend on the block size.
void testVoice()
{
    using VI = insv::VoiceIsolator;
    std::string why;
    CHECK(VI::available(&why));
    if (!VI::available())
        std::printf("voice: model not available (%s)\n", why.c_str());
    VI probe(kRate, kCh, VI::Kind::VoiceOnly);
    const int lat = probe.latency();
    CHECK(lat > 0 && lat < 4 * kRate);
    const int len = 12 * kRate;
    const auto noise = interleave(whiteNoise(len, 0.05f, 41));
    auto run = [&](VI::Kind kind, float strength, int block) {
        VI v(kRate, kCh, kind);
        v.setStrength(strength);
        std::vector<float> out(noise.size());
        for (int i = 0; i < len; i += block) {
            const int n = std::min(block, len - i);
            v.process(noise.data() + static_cast<size_t>(i) * kCh, out.data() + static_cast<size_t>(i) * kCh, n);
        }
        return out;
    };
    auto levelDb = [&](const std::vector<float>& out) { // 6-8 s, vs the input
        double pin = 0, pout = 0;
        for (int i = 6 * kRate; i < 8 * kRate; ++i) {
            pin += double(noise[static_cast<size_t>(i - lat) * kCh]) * noise[static_cast<size_t>(i - lat) * kCh];
            pout += double(out[static_cast<size_t>(i) * kCh]) * out[static_cast<size_t>(i) * kCh];
        }
        return 10.0 * std::log10(pout / pin);
    };
    const auto voiceOnly = run(VI::Kind::VoiceOnly, 1.0f, 4096);
    const double onlyDb = levelDb(voiceOnly);
    const double ambDb = levelDb(run(VI::Kind::VoiceAmbience, 0.5f, 4096));
    std::printf("voice: noise alone, Voice only %+.1f dB, Voice + ambience (12 dB) %+.1f dB\n", onlyDb,
                ambDb);
    CHECK(onlyDb < -25.0);
    CHECK(ambDb < -10.0 && ambDb > -13.0);
    CHECK(delayedError(noise, run(VI::Kind::VoiceOnly, 0.0f, 4096), lat + kRate, lat) < 1e-4f);
    const auto odd = run(VI::Kind::VoiceOnly, 1.0f, 777);
    float diff = 0.0f;
    for (size_t i = 0; i < odd.size(); ++i)
        diff = std::max(diff, std::abs(odd[i] - voiceOnly[i]));
    CHECK(diff < 1e-6f);
}
#endif

void testReaderWithWind(const std::string& sine5k, insv::NoiseMode mode, float tol = 2e-3f,
                        float strength = 0.7f)
{
    // ClipAudioReader keeps sample alignment with the filter on: after a
    // seek, and across switching it off. (5 kHz passes the filter -- Hiss /
    // hum trims its far skirt a little, hence `tol`.)
    auto expect = [](int64_t i) {
        return static_cast<float>(std::lround(0.5 * std::sin(i * 2.0 * std::numbers::pi * 5000.0 / kRate) * 32768.0) /
                                  32768.0);
    };
    insv::ClipAudioReader r(kRate, kCh);
    CHECK(r.open(sine5k, -1));
    r.setNoiseReduction({mode, strength});
    std::vector<float> buf(3000 * kCh);
    auto matches = [&](int64_t from, int n, int skip = 0) {
        float err = 0.0f;
        for (int i = skip; i < n; ++i)
            err = std::max(err, std::abs(buf[static_cast<size_t>(i) * kCh] - expect(from + i)));
        return err < tol;
    };
    r.seek(40000);
    r.read(buf.data(), 3000);
    CHECK(matches(40000, 3000));
    CHECK(r.position() == 43000);
    r.read(buf.data(), 1000); // continues
    CHECK(matches(43000, 1000));
    r.seek(0); // warm-up before the file's start
    r.read(buf.data(), 3000);
    CHECK(matches(0, 3000, 1500)); // (the abrupt onset itself is filtered)
    r.setNoiseReduction({insv::NoiseMode::Off, 0.7f});
    CHECK(r.position() == 3000);
    r.read(buf.data(), 1000);
    CHECK(matches(3000, 1000));
}

} // namespace

int main()
{
#ifdef INSV_HAVE_VOICE
    // The DeepFilterNet model as the app finds it (ORBIS_DFN_MODEL_DIR wins).
    if (!std::getenv("ORBIS_DFN_MODEL_DIR"))
        insv::VoiceIsolator::setModelDirectory(core::paths::modelDir("deepfilternet3"));
#endif
    const auto dir = std::filesystem::temp_directory_path() / "orbis_audio_mix_test";
    std::filesystem::create_directories(dir);
    const std::string dc = (dir / "dc.wav").string();
    const std::string ramp = (dir / "ramp.wav").string();
    writeWav(dc, kRate, [](int) { return 0.25; });
    writeWav(ramp, 30000, [](int i) { return i / 32768.0; });

    testDemuxDuration(dc);
    testPlacementGainFades(dc);
    testSeekAndSource(ramp);
    testMissingAndMuted(dc);
    testLimiter();
    const std::string burst = (dir / "burst.wav").string();
    writeWav(burst, kRate, [](int i) {
        // A 500 Hz sine for the burst (a square wave would ring in the
        // resampler and peak above its level).
        return i < 19200 ? 0.0 : i < 28800 ? 0.5 * std::sin(i * 2.0 * std::numbers::pi * 500.0 / kRate) : 0.1;
    });
    testPeaks(burst);

    testWindLatencyAndPassBand();
    testWindBlockSizes();
    testWindStrengthZero();
    testWindKeepsEngineCutsWind();
    testGeneralWind();
    testHissHum();
#ifdef INSV_HAVE_VOICE
    testVoice();
#endif
    const std::string sine5k = (dir / "sine5k.wav").string();
    writeWav(sine5k, 3 * kRate / 2, [](int i) { return 0.5 * std::sin(i * 2.0 * std::numbers::pi * 5000.0 / kRate); });
    testReaderWithWind(sine5k, insv::NoiseMode::MotorcycleWind);
    testReaderWithWind(sine5k, insv::NoiseMode::Wind);
    testReaderWithWind(sine5k, insv::NoiseMode::Hiss, 0.015f);
    const std::string hissHum = (dir / "hisshum.wav").string();
    {
        const auto sig = hissHumSignal(4 * kRate);
        writeWav(hissHum, 4 * kRate, [&](int i) { return static_cast<double>(sig[static_cast<size_t>(i)]); });
    }
    testHissHumAtFileStart(hissHum);
#ifdef INSV_HAVE_VOICE
    // The 5 kHz tone isn't speech, so at any strength the model would take
    // it out: alignment is checked at strength 0 (the same path, unchanged).
    testReaderWithWind(sine5k, insv::NoiseMode::Voice, 2e-3f, 0.0f);
#endif

    std::filesystem::remove_all(dir);
    if (g_failures == 0)
        std::printf("audio_mix_test: all checks passed\n");
    else
        std::printf("audio_mix_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
