// SPDX-License-Identifier: MIT OR Apache-2.0
#include "VoiceIsolator.h"
#include "Rdft.h"

// ONNX Runtime is loaded on first use (dlopen), not linked: the app starts
// without it installed, and Debian's build prints ~2000 lines of "Schema
// error: ... already registered" while loading (harmless: its ONNX operator
// schemas are registered twice), which go to /dev/null that way.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <numbers>
#include <vector>

namespace insv {

namespace {

using cfloat = std::complex<float>;

// DeepFilterNet 3's config.ini.
constexpr int kRate = 48000;
constexpr int kN = 960;  // STFT size (20 ms)
constexpr int kH = 480;  // hop (10 ms): one model frame
constexpr int kBins = kN / 2 + 1;
constexpr int kErb = 32; // ERB bands
constexpr int kDf = 96;  // deep-filtered bins (to 4.8 kHz)
constexpr int kOrder = 5;
constexpr int kAhead = 2; // the model's lookahead (conv and DF alike)
constexpr float kAlpha = 0.99f; // feature normalisation, tau 1 s (libDF's rounding)

// Chunking (see the header).
constexpr int kBlock = 300;   // output frames per model run
constexpr int kFade = 50;     // crossfaded into the next run
constexpr int kContext = 200; // frames run in before a chunk
// A chunk runs once its last frame's lookahead is in; until then the output
// comes from the chunk before, which ends kBlock + kFade + 3 frames earlier.
constexpr int kLatency = (kBlock + kFade + 3) * kH;

// libDF's ERB filter bank: band widths in bins, summing to kBins.
std::vector<int> erbBands()
{
    auto f2e = [](double f) { return 9.265 * std::log(1.0 + f / (24.7 * 9.265)); };
    auto e2f = [](double e) { return 24.7 * 9.265 * (std::exp(e / 9.265) - 1.0); };
    const double fw = static_cast<double>(kRate) / kN;
    const double step = f2e(kRate / 2) / kErb;
    std::vector<int> w(kErb);
    int prev = 0, over = 0;
    for (int i = 1; i <= kErb; ++i) {
        const int fb = static_cast<int>(std::lround(e2f(i * step) / fw));
        int n = fb - prev - over;
        if (n < 2) {
            over = 2 - n;
            n = 2;
        } else {
            over = 0;
        }
        w[i - 1] = n;
        prev = fb;
    }
    w.back() += 1;
    int sum = 0;
    for (int n : w)
        sum += n;
    if (sum > kBins)
        w.back() -= sum - kBins;
    return w;
}

// --- the model, loaded once per process ---

std::mutex g_dirMutex;
std::string g_modelDir;

std::string modelDir()
{
    std::lock_guard<std::mutex> lock(g_dirMutex);
    if (!g_modelDir.empty())
        return g_modelDir;
    if (const char* e = std::getenv("ORBIS_DFN_MODEL_DIR"))
        return e;
    return {};
}

constexpr const char* kFiles[3] = {"enc.onnx", "erb_dec.onnx", "df_dec.onnx"};

// Runs `f` with stderr pointed at /dev/null.
template <typename F>
void quietly(F&& f)
{
    std::fflush(stderr);
    const int saved = dup(2);
    const int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (saved >= 0 && null >= 0)
        dup2(null, 2);
    if (null >= 0)
        close(null);
    try {
        f();
    } catch (...) {
        std::fflush(stderr);
        if (saved >= 0) {
            dup2(saved, 2);
            close(saved);
        }
        throw;
    }
    std::fflush(stderr);
    if (saved >= 0) {
        dup2(saved, 2);
        close(saved);
    }
}

// dlopen()s ONNX Runtime and hands its API to the C++ wrapper. Empty on
// success, else why not.
std::string loadRuntime()
{
    // Debian's soname is the 1.x version, which ORT_API_VERSION is.
    const std::string names[] = {
#ifdef INSV_ORT_LIBRARY
        INSV_ORT_LIBRARY, // the one found at build time
#endif
        "libonnxruntime.so.1." + std::to_string(ORT_API_VERSION), "libonnxruntime.so.1",
        "libonnxruntime.so"};
    void* lib = nullptr;
    std::string err;
    quietly([&] {
        for (const std::string& n : names)
            if ((lib = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL)))
                break;
        if (!lib)
            err = dlerror();
    });
    if (!lib)
        return "can't load ONNX Runtime: " + err;
    using GetApiBase = const OrtApiBase* (*)();
    const auto base = reinterpret_cast<GetApiBase>(dlsym(lib, "OrtGetApiBase"));
    const OrtApi* api = base ? base()->GetApi(ORT_API_VERSION) : nullptr;
    if (!api)
        return "ONNX Runtime too old (need API version " + std::to_string(ORT_API_VERSION) + ")";
    Ort::InitApi(api);
    return {};
}

struct Model {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> enc, erb, df;
    std::string error; // empty when loaded
};

const Model& model()
{
    static const std::unique_ptr<Model> m = [] {
        auto md = std::make_unique<Model>();
        std::string why;
        if (!VoiceIsolator::available(&why)) {
            md->error = why;
            return md;
        }
        if (const std::string e = loadRuntime(); !e.empty()) {
            md->error = e;
            std::fprintf(stderr, "VoiceIsolator: %s -- voice modes pass audio through\n", e.c_str());
            return md;
        }
        try {
            Ort::SessionOptions so;
            // One thread each: several clips may run at once, and one core
            // already does ~80x real time.
            so.SetIntraOpNumThreads(1);
            so.SetInterOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            const std::filesystem::path dir = modelDir();
            quietly([&] {
                md->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "orbis-dfn");
                md->enc = std::make_unique<Ort::Session>(*md->env, (dir / kFiles[0]).c_str(), so);
                md->erb = std::make_unique<Ort::Session>(*md->env, (dir / kFiles[1]).c_str(), so);
                md->df = std::make_unique<Ort::Session>(*md->env, (dir / kFiles[2]).c_str(), so);
            });
        } catch (const std::exception& e) {
            md->enc.reset();
            md->erb.reset();
            md->df.reset();
            md->error = e.what();
        }
        if (!md->error.empty())
            std::fprintf(stderr, "VoiceIsolator: no model (%s) -- voice modes pass audio through\n",
                         md->error.c_str());
        return md;
    }();
    return *m;
}

} // namespace

struct VoiceIsolator::Impl {
    const int channels;
    const bool rateOk;
    const std::vector<int> erb = erbBands();
    std::vector<float> win;
    Rdft tx{kN, true};
    float lim = 1.0f; // share of the original mixed back, 10^(-atten/20)

    // Input per channel from absolute frame `inBase` on.
    std::vector<std::vector<float>> in;
    int64_t inBase = 0;
    int64_t inCount = 0;

    // Analysed STFT frames from `frameBase` on. Frame t covers input
    // [(t-1)H, (t+1)H) (libDF's framing: before the first input, silence).
    struct Frame {
        std::vector<cfloat> spec;         // channels x kBins
        std::array<float, kErb> erbFeat;  // normalised ERB-band dB
        std::array<cfloat, kDf> dfFeat;   // unit-normalised low bins
    };
    std::deque<Frame> frames;
    int64_t frameBase = 0;
    int64_t nextFrame = 0; // next to analyse
    std::array<float, kErb> meanState{};
    std::array<float, kDf> unitState{};

    // Chunks: the next one's first output frame; the last one's tail,
    // waiting to be crossfaded into it.
    int64_t nextChunk = 0;
    std::vector<std::vector<cfloat>> pending; // kFade x (channels x kBins)
    bool havePending = false;

    // Synthesis (overlap-add) and the output queue.
    std::vector<std::vector<float>> synthMem; // per channel, kH
    std::vector<float> ready;                 // finished output, interleaved
    size_t readyPos = 0;
    int64_t outCount = 0;

    // Model I/O scratch.
    std::vector<float> featErb, featSpec;
    std::vector<cfloat> y;             // one frame, channels x kBins
    std::vector<float> frameOut;       // one synthesised frame, channels x kN

    Impl(int rate, int ch)
        : channels(std::max(1, ch))
        , rateOk(rate == kRate)
    {
        win.resize(kN);
        for (int i = 0; i < kN; ++i) {
            const double s = std::sin(std::numbers::pi * (i + 0.5) / kN);
            win[i] = static_cast<float>(std::sin(0.5 * std::numbers::pi * s * s));
        }
        in.assign(channels, {});
        synthMem.assign(channels, std::vector<float>(kH, 0.0f));
        y.resize(static_cast<size_t>(channels) * kBins);
        frameOut.resize(static_cast<size_t>(channels) * kN);
        reset();
    }

    void reset()
    {
        for (auto& c : in)
            c.clear();
        inBase = inCount = 0;
        frames.clear();
        frameBase = nextFrame = 0;
        // libDF's initial normalisation states.
        for (int b = 0; b < kErb; ++b)
            meanState[b] = -60.0f - 30.0f * b / (kErb - 1);
        for (int f = 0; f < kDf; ++f)
            unitState[f] = 0.001f - 0.0009f * f / (kDf - 1);
        nextChunk = 0;
        pending.clear();
        havePending = false;
        for (auto& m : synthMem)
            std::fill(m.begin(), m.end(), 0.0f);
        ready.clear();
        readyPos = 0;
        outCount = 0;
    }

    float inputAt(int c, int64_t i) const
    {
        const int64_t k = i - inBase;
        const auto& v = in[c];
        return (k >= 0 && k < static_cast<int64_t>(v.size())) ? v[static_cast<size_t>(k)] : 0.0f;
    }

    const Frame& frameAt(int64_t t) const { return frames[static_cast<size_t>(t - frameBase)]; }

    void analyse()
    {
        const int64_t t = nextFrame++;
        Frame fr;
        fr.spec.resize(static_cast<size_t>(channels) * kBins);
        const float wnorm = 2.0f * kH / (static_cast<float>(kN) * kN); // libDF
        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < kN; ++i)
                tx.real[i] = inputAt(c, (t - 1) * kH + i) * win[i];
            tx.forward();
            for (int k = 0; k < kBins; ++k)
                fr.spec[static_cast<size_t>(c) * kBins + k] = cfloat(tx.cplx[k].re, tx.cplx[k].im) * wnorm;
        }
        // Features from the channels' average (the X5's are dual mono).
        std::array<cfloat, kBins> avg;
        for (int k = 0; k < kBins; ++k) {
            cfloat s = 0.0f;
            for (int c = 0; c < channels; ++c)
                s += fr.spec[static_cast<size_t>(c) * kBins + k];
            avg[k] = s / static_cast<float>(channels);
        }
        int k0 = 0;
        for (int b = 0; b < kErb; ++b) {
            float p = 0.0f;
            for (int k = k0; k < k0 + erb[b]; ++k)
                p += std::norm(avg[k]);
            k0 += erb[b];
            const float db = 10.0f * std::log10(p / erb[b] + 1e-10f);
            meanState[b] = db * (1 - kAlpha) + meanState[b] * kAlpha;
            fr.erbFeat[b] = (db - meanState[b]) / 40.0f;
        }
        for (int f = 0; f < kDf; ++f) {
            unitState[f] = std::abs(avg[f]) * (1 - kAlpha) + unitState[f] * kAlpha;
            fr.dfFeat[f] = avg[f] / std::sqrt(unitState[f]);
        }
        frames.push_back(std::move(fr));
    }

    // Runs the model over frames [t0, t1): ERB gains `m` (T x kErb) and DF
    // coefficients `coefs` (T x kDf x kOrder x re/im). False without a model
    // (or on an error: that chunk passes through).
    bool runModel(int64_t t0, int64_t t1, std::vector<float>& m, std::vector<float>& coefs)
    {
        const Model& md = model();
        if (!md.error.empty() || !rateOk)
            return false;
        const int64_t T = t1 - t0;
        featErb.resize(static_cast<size_t>(T) * kErb);
        featSpec.resize(static_cast<size_t>(2 * T) * kDf);
        for (int64_t t = 0; t < T; ++t) {
            const Frame& fr = frameAt(t0 + t);
            std::copy(fr.erbFeat.begin(), fr.erbFeat.end(), featErb.begin() + t * kErb);
            for (int f = 0; f < kDf; ++f) {
                featSpec[static_cast<size_t>(t * kDf + f)] = fr.dfFeat[f].real();
                featSpec[static_cast<size_t>((T + t) * kDf + f)] = fr.dfFeat[f].imag();
            }
        }
        try {
            const auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 4> erbShape{1, 1, T, kErb}, specShape{1, 2, T, kDf};
            std::array<Ort::Value, 2> encIn{
                Ort::Value::CreateTensor<float>(mem, featErb.data(), featErb.size(), erbShape.data(), 4),
                Ort::Value::CreateTensor<float>(mem, featSpec.data(), featSpec.size(), specShape.data(), 4)};
            const char* encInNames[] = {"feat_erb", "feat_spec"};
            const char* encOutNames[] = {"e0", "e1", "e2", "e3", "emb", "c0", "lsnr"};
            auto enc = md.enc->Run(Ort::RunOptions{nullptr}, encInNames, encIn.data(), 2, encOutNames, 7);
            // The decoders take the encoder's outputs; views onto them, since
            // emb goes to both.
            auto view = [&](Ort::Value& v) {
                auto info = v.GetTensorTypeAndShapeInfo();
                const auto shape = info.GetShape();
                return Ort::Value::CreateTensor<float>(mem, v.GetTensorMutableData<float>(),
                                                       info.GetElementCount(), shape.data(), shape.size());
            };
            std::array<Ort::Value, 5> erbIn{view(enc[4]), view(enc[3]), view(enc[2]), view(enc[1]), view(enc[0])};
            const char* erbInNames[] = {"emb", "e3", "e2", "e1", "e0"};
            const char* erbOutNames[] = {"m"};
            auto mOut = md.erb->Run(Ort::RunOptions{nullptr}, erbInNames, erbIn.data(), 5, erbOutNames, 1);
            std::array<Ort::Value, 2> dfIn{view(enc[4]), view(enc[5])};
            const char* dfInNames[] = {"emb", "c0"};
            const char* dfOutNames[] = {"coefs"};
            auto cOut = md.df->Run(Ort::RunOptions{nullptr}, dfInNames, dfIn.data(), 2, dfOutNames, 1);

            const float* mp = mOut[0].GetTensorData<float>();
            const float* cp = cOut[0].GetTensorData<float>();
            m.assign(mp, mp + T * kErb);
            coefs.assign(cp, cp + T * kDf * kOrder * 2);
            return true;
        } catch (const std::exception& e) {
            static std::once_flag once;
            std::call_once(once, [&] { std::fprintf(stderr, "VoiceIsolator: model run failed: %s\n", e.what()); });
            return false;
        }
    }

    // Output frames [a, a + kBlock) (and the next kFade, kept for the
    // crossfade), from one model run over [a + 2 - kContext, a + kBlock +
    // kFade + 2).
    void runChunk(int64_t a)
    {
        const int64_t t0 = std::max<int64_t>(0, a + kAhead - kContext);
        const int64_t t1 = a + kBlock + kFade + kAhead;
        std::vector<float> m, coefs;
        const bool enhanced = runModel(t0, t1, m, coefs);

        std::vector<std::vector<cfloat>> tail(kFade);
        const size_t nb = static_cast<size_t>(channels) * kBins;
        for (int64_t j = a; j < a + kBlock + kFade; ++j) {
            const Frame& fr = frameAt(j);
            if (!enhanced) {
                std::copy(fr.spec.begin(), fr.spec.end(), y.begin());
            } else {
                const int64_t k = j + kAhead - t0; // the model runs kAhead frames ahead
                const float* g = m.data() + k * kErb;
                for (int c = 0; c < channels; ++c) {
                    const cfloat* s = fr.spec.data() + static_cast<size_t>(c) * kBins;
                    cfloat* o = y.data() + static_cast<size_t>(c) * kBins;
                    int bin = 0;
                    for (int b = 0; b < kErb; ++b)
                        for (int e = 0; e < erb[b]; ++e, ++bin)
                            o[bin] = s[bin] * g[b];
                    // Deep filter: 5 taps over frames j-2 .. j+2 (the
                    // unmasked spectrum), replacing the gains below 4.8 kHz.
                    const float* cf = coefs.data() + k * kDf * kOrder * 2;
                    for (int f = 0; f < kDf; ++f)
                        o[f] = 0.0f;
                    for (int tap = 0; tap < kOrder; ++tap) {
                        const int64_t src = j - kAhead + tap;
                        if (src < frameBase || src < 0)
                            continue;
                        const cfloat* ss = frameAt(src).spec.data() + static_cast<size_t>(c) * kBins;
                        for (int f = 0; f < kDf; ++f) {
                            const float* w = cf + (f * kOrder + tap) * 2;
                            o[f] += ss[f] * cfloat(w[0], w[1]);
                        }
                    }
                    for (int bin2 = 0; bin2 < kBins; ++bin2)
                        o[bin2] = o[bin2] * (1.0f - lim) + s[bin2] * lim;
                }
            }
            if (havePending && j < a + kFade) {
                const float w = (static_cast<float>(j - a) + 0.5f) / kFade;
                const auto& p = pending[static_cast<size_t>(j - a)];
                for (size_t i = 0; i < nb; ++i)
                    y[i] = w * y[i] + (1.0f - w) * p[i];
            }
            if (j < a + kBlock)
                synthesise(j);
            else
                tail[static_cast<size_t>(j - a - kBlock)] = y;
        }
        pending = std::move(tail);
        havePending = true;
        nextChunk = a + kBlock;
    }

    // Overlap-add frame j (in `y`): finishes output [(j-1)H, jH).
    void synthesise(int64_t j)
    {
        for (int c = 0; c < channels; ++c) {
            const cfloat* s = y.data() + static_cast<size_t>(c) * kBins;
            for (int k = 0; k < kBins; ++k) {
                tx.cplx[k].re = s[k].real();
                tx.cplx[k].im = s[k].imag();
            }
            tx.inverse(); // scaled 1/N: libDF's inverse isn't
            float* fo = frameOut.data() + static_cast<size_t>(c) * kN;
            for (int i = 0; i < kN; ++i)
                fo[i] = tx.real[i] * kN * win[i];
            auto& mem = synthMem[c];
            for (int i = 0; i < kH; ++i) {
                const float v = fo[i] + mem[i];
                mem[i] = fo[kH + i];
                fo[i] = v;
            }
        }
        if (j < 1)
            return; // before the first input
        for (int i = 0; i < kH; ++i)
            for (int c = 0; c < channels; ++c)
                ready.push_back(frameOut[static_cast<size_t>(c) * kN + i]);
    }

    void push(const float* pcm, int n)
    {
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
                in[c].push_back(pcm[static_cast<size_t>(i) * channels + c]);
        inCount += n;
    }

    void run()
    {
        while (inCount >= (nextFrame + 1) * kH)
            analyse();
        while (nextFrame >= nextChunk + kBlock + kFade + kAhead)
            runChunk(nextChunk);
        // Keep what the next chunk needs: its run-in and the DF taps.
        const int64_t keepFrame = std::max<int64_t>(0, nextChunk + kAhead - kContext);
        while (frameBase < keepFrame && !frames.empty()) {
            frames.pop_front();
            ++frameBase;
        }
        const int64_t keepIn = (nextFrame - 1) * kH; // the next frame's first sample
        if (keepIn - inBase > 1 << 16) {
            const int64_t drop = keepIn - inBase;
            for (auto& c : in)
                c.erase(c.begin(), c.begin() + drop);
            inBase += drop;
        }
    }

    void pop(float* out, int n)
    {
        const size_t ch = static_cast<size_t>(channels);
        for (int i = 0; i < n; ++i, ++outCount) {
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

VoiceIsolator::VoiceIsolator(int sampleRate, int channels, Kind kind)
    : m_impl(std::make_unique<Impl>(sampleRate, channels))
    , m_kind(kind)
    , m_strength(NoiseReduction::defaultStrength(kind == Kind::VoiceOnly ? NoiseMode::Voice
                                                                         : NoiseMode::VoiceAmbience))
{
    setStrength(m_strength);
}

VoiceIsolator::~VoiceIsolator() = default;

bool VoiceIsolator::available(std::string* why)
{
    const std::filesystem::path dir = modelDir();
    if (dir.empty()) {
        if (why)
            *why = "no model directory set";
        return false;
    }
    for (const char* f : kFiles) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dir / f, ec)) {
            if (why)
                *why = "missing " + (dir / f).string();
            return false;
        }
    }
    return true;
}

bool VoiceIsolator::ready(std::string* why)
{
    const Model& m = model();
    if (why)
        *why = m.error;
    return m.error.empty();
}

void VoiceIsolator::setModelDirectory(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(g_dirMutex);
    g_modelDir = dir;
}

void VoiceIsolator::setStrength(float strength)
{
    m_strength = std::clamp(strength, 0.0f, 1.0f);
    m_impl->lim = std::pow(10.0f, -attenuationDb() / 20.0f);
}

float VoiceIsolator::attenuationDb() const
{
    return (m_kind == Kind::VoiceOnly ? 60.0f : 24.0f) * m_strength;
}

void VoiceIsolator::reset()
{
    m_impl->reset();
}

int VoiceIsolator::latency() const
{
    return kLatency;
}

void VoiceIsolator::process(const float* in, float* out, int frames)
{
    if (!m_impl->tx.ok()) { // no FFT (never seen): silence
        std::fill(out, out + static_cast<size_t>(frames) * m_impl->channels, 0.0f);
        return;
    }
    m_impl->push(in, frames);
    m_impl->run();
    m_impl->pop(out, frames);
}

} // namespace insv
