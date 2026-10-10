// tools/audio_filter_bench: test bench for the audio noise filters. Decodes
// one audio stream of an .insv to a WAV and runs the app's filters on it;
// `analyze` scores any output against the raw WAV per band and on / between
// the engine harmonics. The other candidates
// (DeepFilterNet, FFmpeg afftdn) are run on the raw WAV by run_all.sh.
//
//   audio_filter_bench extract <clip.insv> <outdir> [--stream N] [--start S]
//                      [--dur S] [--strength 0..1] [--fft N]
//     raw_sN.wav, and wind_sN.wav through the app's path (ClipAudioReader +
//     insv::WindReducer, "MC: Wind reduction")
//   audio_filter_bench analyze <raw.wav> <test.wav> [--fft N]
//   audio_filter_bench general|hiss|voice|voiceamb <in.wav> <out.wav> [--strength 0..1]
//     (the app's "Wind reduction" / "Hiss / hum" / "Voice only" /
//     "Voice + ambience" filter on a WAV)
//
// The X5 streams (camera mic and Mic Air) are dual mono: both channels are
// bit-identical (measured on every sample clip, 2026-09-29), so everything
// here works on channel 0 and writes mono.
//
// This tool's own first wind filter, which insv::WindReducer productised,
// was removed 2026-10-05; its engine-f0 tracker stays as the scorer.

#include "ClipAudioReader.h"
#include "GeneralWindReducer.h"
#include "HissHumReducer.h"
#include "NoiseFilter.h"
#ifdef INSV_HAVE_VOICE
#include "VoiceIsolator.h"
#include "DataPaths.h"
#endif
#include "InsvAudioDemux.h"
#include "Rdft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr int kRate = 48000;

// ---------------------------------------------------------------- WAV I/O

bool writeWav(const std::string& path, const std::vector<float>& mono)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataBytes = uint32_t(mono.size() * 4);
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(1);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(32);
    f.write("data", 4);
    u32(dataBytes);
    f.write(reinterpret_cast<const char*>(mono.data()), dataBytes);
    return bool(f);
}

// Reads PCM16 / float32 WAV, any channel count; returns channel 0.
bool readWav(const std::string& path, std::vector<float>& mono, int& rate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char id[4];
    uint32_t size = 0;
    f.read(id, 4);
    f.read(reinterpret_cast<char*>(&size), 4);
    f.read(id, 4);
    if (std::memcmp(id, "WAVE", 4) != 0)
        return false;
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t r = 0;
    while (f.read(id, 4) && f.read(reinterpret_cast<char*>(&size), 4)) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<char> b(size);
            f.read(b.data(), size);
            std::memcpy(&fmt, &b[0], 2);
            std::memcpy(&channels, &b[2], 2);
            std::memcpy(&r, &b[4], 4);
            std::memcpy(&bits, &b[14], 2);
            if (fmt == 0xFFFE && size >= 26) // WAVE_FORMAT_EXTENSIBLE: subformat
                std::memcpy(&fmt, &b[24], 2);
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (channels == 0)
                return false;
            std::vector<char> b(size);
            f.read(b.data(), size);
            const size_t frameBytes = size_t(channels) * bits / 8;
            const size_t n = size / frameBytes;
            mono.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const char* p = &b[i * frameBytes];
                if (fmt == 3 && bits == 32) {
                    std::memcpy(&mono[i], p, 4);
                } else if (fmt == 1 && bits == 16) {
                    int16_t s;
                    std::memcpy(&s, p, 2);
                    mono[i] = s / 32768.0f;
                } else {
                    return false;
                }
            }
            rate = int(r);
            return true;
        } else {
            f.seekg(size + (size & 1), std::ios::cur);
        }
    }
    return false;
}

// ---------------------------------------------------------------- STFT

// Power spectra of all frames (hop = n/4).
std::vector<std::vector<float>> powerFrames(const std::vector<float>& x, int n)
{
    const int hop = n / 4;
    insv::Rdft tx(n, false);
    const auto win = insv::sqrtHann(n);
    std::vector<std::vector<float>> out;
    for (size_t start = 0; start + n <= x.size(); start += hop) {
        for (int i = 0; i < n; ++i)
            tx.real[i] = x[start + i] * win[i];
        tx.forward();
        std::vector<float> p(n / 2 + 1);
        for (int k = 0; k <= n / 2; ++k)
            p[k] = tx.cplx[k].re * tx.cplx[k].re + tx.cplx[k].im * tx.cplx[k].im;
        out.push_back(std::move(p));
    }
    return out;
}

// ---------------------------------------------------------------- f0 tracker

// Harmonic-sum engine-fundamental tracker on one power spectrum. The
// spectrum is whitened by a smoothed log envelope first so the loud low
// harmonics don't dominate; salience(f0) = mean whitened level at k*f0.
struct F0Tracker {
    int n;
    float binHz;
    float prevF0 = 0.0f;

    F0Tracker(int fftSize) : n(fftSize), binHz(float(kRate) / fftSize) {}

    // Returns f0 (0 = none) and confidence 0..1.
    std::pair<float, float> track(const std::vector<float>& p)
    {
        const int nb = int(p.size());
        std::vector<float> lp(nb), env(nb);
        for (int k = 0; k < nb; ++k)
            lp[k] = 10.0f * std::log10(p[k] + 1e-12f);
        // Envelope: moving average over ~60 Hz.
        const int half = std::max(2, int(30.0f / binHz));
        for (int k = 0; k < nb; ++k) {
            double s = 0;
            int c = 0;
            for (int j = std::max(0, k - half); j <= std::min(nb - 1, k + half); ++j, ++c)
                s += lp[j];
            env[k] = float(s / c);
        }
        auto white = [&](float f) {
            const float b = f / binHz;
            const int k0 = int(b);
            if (k0 + 1 >= nb)
                return 0.0f;
            const float fr = b - k0;
            return (1 - fr) * (lp[k0] - env[k0]) + fr * (lp[k0 + 1] - env[k0 + 1]);
        };
        // Sum (not mean) of the whitened level over the harmonics below
        // 1.5 kHz: the true f0 collects every harmonic peak, a multiple of it
        // only some of them, and a sub-multiple adds the valleys in between
        // (negative after whitening) -- so the sum peaks at the true f0.
        float best = -1e9f, bestF = 0.0f;
        std::vector<float> sal;
        for (float f0 = 18.0f; f0 <= 250.0f; f0 += 0.1f) {
            double s = 0;
            for (float f = f0; f < 1500.0f; f += f0)
                s += std::clamp(white(f), -6.0f, 20.0f);
            float v = float(s);
            // Continuity: penalise jumps (in octaves) from the previous f0.
            if (prevF0 > 0.0f)
                v -= 20.0f * std::fabs(std::log2(f0 / prevF0));
            sal.push_back(v);
            if (v > best) {
                best = v;
                bestF = f0;
            }
        }
        // Confidence: the mean whitened level on the chosen harmonics (dB
        // above the envelope): ~0 for noise, several dB for a clear comb.
        int K = 0;
        double s = 0;
        for (float f = bestF; f < 1500.0f; f += bestF, ++K)
            s += std::clamp(white(f), -6.0f, 20.0f);
        const float conf = std::clamp(float(s / std::max(1, K) - 1.0) / 4.0f, 0.0f, 1.0f);
        if (conf > 0.2f)
            prevF0 = prevF0 > 0.0f ? 0.5f * prevF0 + 0.5f * bestF : bestF;
        return {conf > 0.2f ? bestF : 0.0f, conf};
    }

};

// Protection 0..1 per bin: Gaussian around each harmonic of f0.
void protection(float f0, float conf, float binHz, std::vector<float>& out)
{
    std::fill(out.begin(), out.end(), 0.0f);
    if (f0 <= 0.0f)
        return;
    const int nb = int(out.size());
    for (int k = 1; k * f0 < 4000.0f; ++k) {
        const float c = k * f0 / binHz;
        const float sigma = std::max(0.6f, k * 0.003f * f0 / binHz);
        for (int b = std::max(0, int(c - 3 * sigma)); b <= std::min(nb - 1, int(c + 3 * sigma) + 1);
             ++b) {
            const float d = (b - c) / sigma;
            out[b] = std::max(out[b], conf * std::exp(-0.5f * d * d));
        }
    }
}

// Wind-band weight: 1 below 800 Hz, cosine fade to 0 at 3 kHz.
float windBand(float hz)
{
    if (hz <= 800.0f)
        return 1.0f;
    if (hz >= 3000.0f)
        return 0.0f;
    return 0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * (hz - 800.0f) / 2200.0f);
}

// Engine tracking needs finer frequency resolution than the analysis (the
// harmonics of a ~23 Hz fundamental are ~4 bins apart at 8192): a
// kTrackFft-point power spectrum centred on the analysis frame.
constexpr int kTrackFft = 16384;

struct TrackSpectrum {
    insv::Rdft tx{kTrackFft, false};
    std::vector<float> win = insv::sqrtHann(kTrackFft);
    std::vector<float> p = std::vector<float>(kTrackFft / 2 + 1);

    const std::vector<float>& at(const std::vector<float>& x, long centre)
    {
        const long start = centre - kTrackFft / 2;
        for (int i = 0; i < kTrackFft; ++i) {
            const long j = start + i;
            tx.real[i] = (j >= 0 && j < long(x.size())) ? x[j] * win[i] * win[i] : 0.0f;
        }
        tx.forward();
        for (size_t k = 0; k < p.size(); ++k)
            p[k] = tx.cplx[k].re * tx.cplx[k].re + tx.cplx[k].im * tx.cplx[k].im;
        return p;
    }
};

// ---------------------------------------------------------------- analysis

const float kBands[] = {20, 100, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr int kNumBands = 8;

struct Report {
    double band[kNumBands] = {};
    double harm = 0, nonHarmWind = 0; // wind band, confident frames only
    int frames = 0, confFrames = 0;
};

Report analyze(const std::vector<std::vector<float>>& frames, const std::vector<float>& f0s,
               const std::vector<float>& confs, int n)
{
    const float binHz = float(kRate) / n;
    Report r;
    std::vector<float> prot(n / 2 + 1);
    for (size_t t = 0; t < frames.size(); ++t) {
        const auto& p = frames[t];
        for (int k = 1; k < int(p.size()); ++k) {
            const float hz = k * binHz;
            for (int b = 0; b < kNumBands; ++b)
                if (hz >= kBands[b] && hz < kBands[b + 1])
                    r.band[b] += p[k];
        }
        ++r.frames;
        if (t < f0s.size() && f0s[t] > 0.0f && confs[t] > 0.5f) {
            ++r.confFrames;
            protection(f0s[t], 1.0f, binHz, prot);
            for (int k = 1; k < int(p.size()); ++k) {
                const float w = windBand(k * binHz);
                if (w <= 0.0f)
                    continue;
                if (prot[k] > 0.5f)
                    r.harm += p[k];
                else
                    r.nonHarmWind += p[k];
            }
        }
    }
    return r;
}

double db(double v) { return 10.0 * std::log10(v + 1e-20); }

void printCompare(const Report& ref, const Report& test)
{
    std::printf("band (Hz)        ref dB   test dB   change\n");
    for (int b = 0; b < kNumBands; ++b) {
        const double a = db(ref.band[b] / std::max(1, ref.frames));
        const double c = db(test.band[b] / std::max(1, test.frames));
        std::printf("%5.0f-%-6.0f   %7.1f  %7.1f   %+6.1f\n", kBands[b], kBands[b + 1], a, c, c - a);
    }
    std::printf("engine harmonics (<3 kHz, %d/%d confident frames): %+6.1f dB\n", ref.confFrames,
                ref.frames, db(test.harm) - db(ref.harm));
    std::printf("between harmonics (<3 kHz, same frames):          %+6.1f dB\n",
                db(test.nonHarmWind) - db(ref.nonHarmWind));
}

// f0 track over the reference, so any output is scored on the same bins.
void trackAll(const std::vector<float>& x, int n, std::vector<float>& f0s, std::vector<float>& confs)
{
    F0Tracker tr(kTrackFft);
    TrackSpectrum ts;
    for (size_t start = 0; start + n <= x.size(); start += n / 4) {
        const auto [f0, c] = tr.track(ts.at(x, long(start) + n / 2));
        f0s.push_back(f0);
        confs.push_back(c);
    }
}

const char kUsage[] =
    "usage: audio_filter_bench extract <clip.insv> <outdir> [--stream N] [--start S] [--dur S]\n"
    "                          [--strength 0..1] [--fft N]\n"
    "         raw_sN.wav, and wind_sN.wav through the app's \"MC: Wind reduction\"\n"
    "       audio_filter_bench analyze <raw.wav> <test.wav> [--fft N]\n"
    "         score a filtered WAV against the raw one\n"
    "       audio_filter_bench general|hiss|voice|voiceamb <in.wav> <out.wav> [--strength 0..1]\n"
    "         the app's \"Wind reduction\" / \"Hiss / hum\" / \"Voice only\" /\n"
    "         \"Voice + ambience\" filter on a WAV\n";

int usage()
{
    std::fputs(kUsage, stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
#ifdef INSV_HAVE_VOICE
    // The DeepFilterNet model as the app finds it (ORBIS_DFN_MODEL_DIR wins).
    if (!std::getenv("ORBIS_DFN_MODEL_DIR"))
        insv::VoiceIsolator::setModelDirectory(core::paths::modelDir("deepfilternet3"));
#endif
    if (argc < 4)
        return usage();
    const std::string cmd = argv[1];
    int stream = -1, fft = 8192;
    double startS = 0, durS = 60;
    float strength = 0.7f;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const char* v = argv[i + 1];
        if (a == "--stream")
            stream = std::atoi(v);
        else if (a == "--start")
            startS = std::atof(v);
        else if (a == "--dur")
            durS = std::atof(v);
        else if (a == "--strength")
            strength = float(std::atof(v));
        else if (a == "--fft")
            fft = std::atoi(v);
        else
            return usage();
    }

    if (cmd == "spectrum") { // debug: peaks of a 1 s average spectrum at --start
        std::vector<float> x;
        int r = 0;
        if (!readWav(argv[2], x, r))
            return 1;
        const size_t a = size_t(startS * kRate), b = std::min(x.size(), a + kRate);
        std::vector<float> seg(x.begin() + a, x.begin() + b);
        const auto frames = powerFrames(seg, fft);
        std::vector<float> avg(fft / 2 + 1, 0.0f);
        for (const auto& p : frames)
            for (size_t k = 0; k < p.size(); ++k)
                avg[k] += p[k];
        const float binHz = float(kRate) / fft;
        for (int k = 2; k < int(800 / binHz); ++k) {
            const float l = 10 * std::log10(avg[k] + 1e-12f);
            double e = 0;
            const int h = int(40 / binHz);
            for (int j = k - h; j <= k + h; ++j)
                e += 10 * std::log10(avg[std::max(1, j)] + 1e-12f);
            e /= 2 * h + 1;
            if (avg[k] > avg[k - 1] && avg[k] >= avg[k + 1] && l - e > 3)
                std::printf("%.1f Hz  %+.1f dB over env (%.1f dB)\n", k * binHz, l - e, l);
        }
        return 0;
    }
    if (cmd == "analyze") {
        std::vector<float> ref, test;
        int r1 = 0, r2 = 0;
        if (!readWav(argv[2], ref, r1) || !readWav(argv[3], test, r2) || r1 != kRate || r2 != kRate) {
            std::fprintf(stderr, "can't read both WAVs as %d Hz\n", kRate);
            return 1;
        }
        const size_t len = std::min(ref.size(), test.size());
        ref.resize(len);
        test.resize(len);
        const auto fr = powerFrames(ref, fft), ft = powerFrames(test, fft);
        std::vector<float> f0s, confs;
        trackAll(ref, fft, f0s, confs);
        printCompare(analyze(fr, f0s, confs, fft), analyze(ft, f0s, confs, fft));
        return 0;
    }
    if (cmd == "general" || cmd == "hiss" || cmd == "voice" || cmd == "voiceamb") { // the app's filter on a WAV
        std::vector<float> x;
        int r = 0;
        if (!readWav(argv[2], x, r) || r != kRate) {
            std::fprintf(stderr, "can't read %s as %d Hz\n", argv[2], kRate);
            return 1;
        }
        insv::GeneralWindReducer g(kRate, 1);
        insv::HissHumReducer h(kRate, 1);
        std::unique_ptr<insv::NoiseFilter> v;
        if (cmd == "voice" || cmd == "voiceamb") {
            v = insv::makeNoiseFilter(cmd == "voice" ? insv::NoiseMode::Voice : insv::NoiseMode::VoiceAmbience,
                                      kRate, 1);
            if (!v) {
                std::fprintf(stderr, "built without the voice modes\n");
                return 1;
            }
        }
        insv::NoiseFilter& f = v ? *v : cmd == "hiss" ? static_cast<insv::NoiseFilter&>(h) : g;
        f.setStrength(strength);
        const int lat = f.latency();
        std::vector<float> in(x), out(x.size() + lat);
        in.resize(x.size() + lat, 0.0f);
        constexpr int kBlock = 4096;
        // As ClipAudioReader at a file's start: learn from the opening
        // warm-up, then rewind.
        const size_t learn = std::min(x.size(), size_t(f.warmupSeconds() * kRate));
        for (size_t i = 0; i < learn; i += kBlock)
            f.process(in.data() + i, out.data() + i, int(std::min<size_t>(kBlock, learn - i)));
        f.rewind();
        std::vector<std::string> notes;
        for (size_t i = 0; i < in.size(); i += kBlock) {
            const int n = int(std::min<size_t>(kBlock, in.size() - i));
            f.process(in.data() + i, out.data() + i, n);
            char buf[32];
            if (v)
                buf[0] = 0;
            else if (cmd == "hiss")
                std::snprintf(buf, sizeof buf, " %.1f(%.2f)", h.humHz(), h.humConfidence());
            else
                std::snprintf(buf, sizeof buf, " %.2f", g.lastWindConfidence());
            notes.push_back(buf);
        }
        out.erase(out.begin(), out.begin() + lat);
        if (!writeWav(argv[3], out))
            return 1;
        const int per2s = std::max(1, int(2.0 * kRate / kBlock));
        std::printf(v ? "" : cmd == "hiss" ? "hum Hz (confidence) every 2 s:" : "wind confidence every 2 s:");
        for (size_t i = 0; i < notes.size(); i += per2s)
            std::printf("%s", notes[i].c_str());
        std::printf("\n");
        const auto fr = powerFrames(x, fft), ft = powerFrames(out, fft);
        std::vector<float> f0s, cs;
        trackAll(x, fft, f0s, cs);
        printCompare(analyze(fr, f0s, cs, fft), analyze(ft, f0s, cs, fft));
        return 0;
    }
    if (cmd != "extract")
        return usage();

    const std::string clip = argv[2], outDir = argv[3];
    insv::InsvAudioDemux demux;
    std::string err;
    if (!demux.open(clip, kRate, 2, err, stream)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    if (startS > 0 && !demux.seekTo(int64_t(startS * 1000), err)) {
        std::fprintf(stderr, "seek: %s\n", err.c_str());
        return 1;
    }
    std::vector<float> chunk, mono;
    double maxLR = 0;
    size_t clipped = 0;
    float peak = 0;
    const size_t want = size_t(durS * kRate);
    double pts = 0;
    while (mono.size() < want) {
        if (!demux.readNext(chunk, nullptr, err, &pts)) {
            if (!err.empty())
                std::fprintf(stderr, "decode: %s\n", err.c_str());
            break;
        }
        // Drop the head before the seek target (readNext starts up to one
        // AAC frame early).
        size_t skip = 0;
        if (mono.empty() && pts < startS)
            skip = size_t((startS - pts) * kRate);
        for (size_t i = skip; i < chunk.size() / 2 && mono.size() < want; ++i) {
            const float l = chunk[2 * i], r = chunk[2 * i + 1];
            maxLR = std::max(maxLR, double(std::fabs(l - r)));
            peak = std::max(peak, std::fabs(l));
            if (std::fabs(l) >= 0.999f)
                ++clipped;
            mono.push_back(l);
        }
    }
    std::printf("stream %d: %.1f s from %.1f s; peak %.1f dBFS, %zu clipped samples, max |L-R| %.2g%s\n",
                stream, mono.size() / double(kRate), startS, 20 * std::log10(peak + 1e-12f), clipped,
                maxLR, maxLR < 1e-4 ? " (dual mono)" : "");

    const std::string tag = "s" + std::to_string(stream < 0 ? 0 : stream);
    const std::string rawPath = outDir + "/raw_" + tag + ".wav";
    const std::string windPath = outDir + "/wind_" + tag + ".wav";

    // The app's "MC: Wind reduction" (ClipAudioReader + insv::WindReducer).
    insv::ClipAudioReader reader(kRate, 2);
    reader.open(clip, stream);
    reader.setNoiseReduction({insv::NoiseMode::MotorcycleWind, strength});
    reader.seek(int64_t(startS * kRate));
    std::vector<float> stereo(mono.size() * 2), wind(mono.size());
    reader.read(stereo.data(), int64_t(mono.size()));
    for (size_t i = 0; i < mono.size(); ++i)
        wind[i] = stereo[2 * i];
    if (!writeWav(rawPath, mono) || !writeWav(windPath, wind)) {
        std::fprintf(stderr, "can't write WAVs to %s\n", outDir.c_str());
        return 1;
    }

    std::vector<float> f0s, confs;
    trackAll(mono, fft, f0s, confs);
    // f0 track summary, one line per ~2 s.
    const double frameSec = double(fft / 4) / kRate;
    const int step = std::max(1, int(2.0 / frameSec));
    std::printf("engine f0 (Hz, confidence) every 2 s:\n ");
    for (size_t t = 0; t < f0s.size(); t += step)
        std::printf(" %.0f(%.1f)", f0s[t], confs[t]);
    std::printf("\n");

    const auto fr = powerFrames(mono, fft), fw = powerFrames(wind, fft);
    std::printf("wind filter (strength %.2f) vs raw:\n", strength);
    printCompare(analyze(fr, f0s, confs, fft), analyze(fw, f0s, confs, fft));
    std::printf("wrote %s, %s\n", rawPath.c_str(), windPath.c_str());
    return 0;
}
