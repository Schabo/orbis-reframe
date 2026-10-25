#include "AudioOutput.h"

#include "../core/Log.h"

// Only what playback needs: the device API on the Linux sound servers (and
// the null device for tests). WASAPI joins the list with a Windows build.
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_JACK
#define MA_ENABLE_NULL
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>

#include <sys/time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

// libpulse's pa_timing_info (pulse/def.h), public and ABI-stable; declared
// here so the build needs no libpulse headers -- miniaudio loads the library
// at run time and so do we.
struct PaTimingInfo {
    timeval timestamp;
    int synchronized_clocks;
    uint64_t sink_usec;
    uint64_t source_usec;
    uint64_t transport_usec;
    int playing;
    int write_index_corrupt;
    int64_t write_index;
    int read_index_corrupt;
    int64_t read_index;
    uint64_t configured_sink_usec;
    uint64_t configured_source_usec;
    uint64_t since_underrun;
};
using PaUpdateTimingInfo = void *(*)(void *stream, void *callback, void *userdata);
using PaGetTimingInfo = const PaTimingInfo *(*)(void *stream);
using PaOperationUnref = void (*)(void *op);

// The clock libpulse stamps its timing info with.
double wallSeconds()
{
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6;
}

// How far a timing reading is extrapolated. Readings arrive with every
// callback (~10 ms apart); a longer silence means the stream stalled, and
// running the clock on would count audio that wasn't heard.
constexpr double kMaxExtrapolationS = 0.05;

// miniaudio's warnings and errors always; its info (the device it opened,
// the conversions) with ORBIS_VERBOSE; its debug lines never.
void onLog(void *, ma_uint32 level, const char *message)
{
    if (level > MA_LOG_LEVEL_INFO || (level == MA_LOG_LEVEL_INFO && !core::log::Verbose()))
        return;
    size_t n = std::strlen(message);
    while (n > 0 && (message[n - 1] == '\n' || message[n - 1] == '\r'))
        --n;
    std::fprintf(stderr, "[audio] %.*s\n", static_cast<int>(n), message);
}

} // namespace

struct AudioOutput::Impl {
    Pull pull;
    ma_log log{};
    ma_context ctx{};
    ma_device dev{};
    bool logOk = false, ctxOk = false, devOk = false;
    std::string description;
    // Output frames per frame the server counts (its rate may differ from
    // ours if miniaudio resamples; then its read index is in its frames).
    double rateRatio = 1.0;
    int serverFrameBytes = 0;
    double bufferFrames = 0.0; // the stream's own buffer, in our frames

    // PulseAudio's timing calls, from the library miniaudio loaded; null on
    // other backends.
    PaUpdateTimingInfo paUpdate = nullptr;
    PaGetTimingInfo paGet = nullptr;
    PaOperationUnref paUnref = nullptr;

    mutable std::mutex mx;
    bool running = false;
    double startedAt = 0.0; // wallSeconds() of the last start()
    uint64_t delivered = 0; // frames pulled
    // The frame clock's floor: everything delivered before the last stop()
    // has been heard or dropped (ALSA drops its buffer on stop).
    uint64_t deliveredAtStop = 0;
    // The last PulseAudio reading: frames audible at its timestamp.
    bool haveReading = false;
    double readingAt = 0.0;
    double readingFrames = 0.0;
    bool readingPlaying = false;

    static void onData(ma_device *dev, void *out, const void *, ma_uint32 frames)
    {
        auto *self = static_cast<Impl *>(dev->pUserData);
        self->pull(static_cast<float *>(out), frames);
        {
            std::lock_guard<std::mutex> lk(self->mx);
            self->delivered += frames;
        }
        if (self->paUpdate)
            self->readPulseClock();
    }

    // Takes the stream's latest timing info and asks for the next. Runs in
    // the data callback because that is miniaudio's PulseAudio main loop
    // thread, the only one where touching the stream is safe.
    void readPulseClock()
    {
        void *stream = dev.pulse.pStreamPlayback;
        if (const PaTimingInfo *ti = paGet(stream); ti && !ti->read_index_corrupt) {
            // What the server has read from the stream, less what is still in
            // the output device: the sum libpulse's pa_stream_get_time()
            // starts from, without the smoothing it adds on top.
            const double at = static_cast<double>(ti->timestamp.tv_sec) +
                              static_cast<double>(ti->timestamp.tv_usec) * 1e-6;
            const double serverFrames =
                static_cast<double>(ti->read_index) / serverFrameBytes -
                static_cast<double>(ti->sink_usec) * 1e-6 * dev.playback.internalSampleRate;
            std::lock_guard<std::mutex> lk(mx);
            if (at >= startedAt) { // a reading from before the last start() is stale
                haveReading = true;
                readingAt = at;
                readingFrames = std::max(0.0, serverFrames * rateRatio);
                readingPlaying = ti->playing != 0;
            }
        }
        if (void *op = paUpdate(stream, nullptr, nullptr))
            paUnref(op);
    }
};

AudioOutput::AudioOutput(Pull pull, Backend backend)
    : d(std::make_unique<Impl>())
{
    d->pull = std::move(pull);

    if (ma_log_init(nullptr, &d->log) == MA_SUCCESS) {
        d->logOk = true;
        ma_log_register_callback(&d->log, ma_log_callback_init(onLog, nullptr));
    }
    ma_context_config cc = ma_context_config_init();
    cc.pLog = d->logOk ? &d->log : nullptr;
    cc.pulse.pApplicationName = "orbis-reframe";
    // An explicit list: left to itself miniaudio would fall back to its null
    // device and "play" into nothing with no sign that there's no sound.
    const ma_backend system[] = {ma_backend_pulseaudio, ma_backend_alsa, ma_backend_jack};
    const ma_backend null[] = {ma_backend_null};
    const bool useNull = backend == Backend::Null;
    ma_result r = ma_context_init(useNull ? null : system, useNull ? 1 : 3, &cc, &d->ctx);
    if (r != MA_SUCCESS) {
        d->description = std::string("none (no sound server found: ") + ma_result_description(r) + ")";
        std::fprintf(stderr, "[audio] no sound output: %s\n", d->description.c_str());
        return;
    }
    d->ctxOk = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kSampleRate;
    // 3 x 10 ms: small, so a seek is heard soon (whatever is buffered plays
    // out first -- see AudioPlayer), and enough for the server's thread.
    cfg.periodSizeInMilliseconds = 10;
    cfg.periods = 3;
    // Hand the server exactly what it asks for. miniaudio's fixed-size
    // callbacks go through a buffer of its own, which it empties on stop():
    // frames pulled and then thrown away, which the clock would count as
    // still to come.
    cfg.noFixedSizedCallback = true;
    cfg.noPreSilencedOutputBuffer = true; // the callback fills every frame
    cfg.dataCallback = Impl::onData;
    cfg.pUserData = d.get();
    cfg.pulse.pStreamNamePlayback = "Playback";
    r = ma_device_init(&d->ctx, &cfg, &d->dev);
    if (r != MA_SUCCESS) {
        d->description = std::string("none (") + ma_get_backend_name(d->ctx.backend) +
                         ": no output device: " + ma_result_description(r) + ")";
        std::fprintf(stderr, "[audio] no sound output: %s\n", d->description.c_str());
        return;
    }
    d->devOk = true;

    const ma_uint32 serverRate = d->dev.playback.internalSampleRate;
    d->rateRatio = serverRate ? static_cast<double>(kSampleRate) / serverRate : 1.0;
    d->serverFrameBytes = static_cast<int>(
        ma_get_bytes_per_frame(d->dev.playback.internalFormat, d->dev.playback.internalChannels));
    d->bufferFrames = static_cast<double>(d->dev.playback.internalPeriodSizeInFrames) *
                      d->dev.playback.internalPeriods * d->rateRatio;

    if (d->ctx.backend == ma_backend_pulseaudio) {
        void *so = d->ctx.pulse.pulseSO;
        d->paUpdate = reinterpret_cast<PaUpdateTimingInfo>(ma_dlsym(nullptr, so, "pa_stream_update_timing_info"));
        d->paGet = reinterpret_cast<PaGetTimingInfo>(ma_dlsym(nullptr, so, "pa_stream_get_timing_info"));
        d->paUnref = reinterpret_cast<PaOperationUnref>(ma_dlsym(nullptr, so, "pa_operation_unref"));
        if (!d->paUpdate || !d->paGet || !d->paUnref)
            d->paUpdate = nullptr;
    }

    char line[512];
    // The device is the default output when the stream opened; the user may
    // move the stream since.
    std::snprintf(line, sizeof line, "%s, opened on %s (%s, %u ch, %u Hz, %u x %u frames)%s",
                  ma_get_backend_name(d->ctx.backend), d->dev.playback.name,
                  ma_get_format_name(d->dev.playback.internalFormat),
                  d->dev.playback.internalChannels, serverRate,
                  d->dev.playback.internalPeriods, d->dev.playback.internalPeriodSizeInFrames,
                  d->paUpdate ? "" : ", clock from frames written");
    d->description = line;
    core::log::Info("[audio] output: %s\n", line);
    if (!d->paUpdate && backend == Backend::System)
        // Without the server's timing the clock can't see the device's own
        // latency: sound may trail the picture by that much.
        std::fprintf(stderr, "[audio] %s has no playback clock; A/V sync ignores the device's latency\n",
                     ma_get_backend_name(d->ctx.backend));
}

AudioOutput::~AudioOutput()
{
    if (d->devOk)
        ma_device_uninit(&d->dev); // stops it first
    if (d->ctxOk)
        ma_context_uninit(&d->ctx);
    if (d->logOk)
        ma_log_uninit(&d->log);
}

bool AudioOutput::ok() const
{
    return d->devOk;
}

const std::string &AudioOutput::description() const
{
    return d->description;
}

void AudioOutput::start()
{
    if (!d->devOk)
        return;
    {
        std::lock_guard<std::mutex> lk(d->mx);
        if (d->running)
            return;
        d->running = true;
        d->startedAt = wallSeconds();
    }
    if (const ma_result r = ma_device_start(&d->dev); r != MA_SUCCESS)
        std::fprintf(stderr, "[audio] can't start the output: %s\n", ma_result_description(r));
}

void AudioOutput::stop()
{
    if (!d->devOk)
        return;
    {
        std::lock_guard<std::mutex> lk(d->mx);
        if (!d->running)
            return;
        d->running = false;
    }
    if (const ma_result r = ma_device_stop(&d->dev); r != MA_SUCCESS)
        std::fprintf(stderr, "[audio] can't stop the output: %s\n", ma_result_description(r));
    std::lock_guard<std::mutex> lk(d->mx); // the callback has stopped
    d->deliveredAtStop = d->delivered;
    // Hold the clock where the stream stopped. Left marked as playing, the
    // last reading would be run on after the next start() -- by the length
    // of the pause, up to kMaxExtrapolationS -- until a fresh one came in,
    // and a seek made then would start the audio that much late.
    if (d->haveReading && d->readingPlaying) {
        const double now = wallSeconds();
        d->readingFrames += std::clamp(now - d->readingAt, 0.0, kMaxExtrapolationS) * kSampleRate;
        d->readingAt = now;
        d->readingPlaying = false;
    }
}

double AudioOutput::playedFrames() const
{
    std::lock_guard<std::mutex> lk(d->mx);
    if (!d->paUpdate) // frames written, less what the stream's buffer may still hold
        return std::max(static_cast<double>(d->deliveredAtStop),
                        static_cast<double>(d->delivered) - d->bufferFrames);
    if (!d->haveReading)
        return 0.0;
    double frames = d->readingFrames;
    if (d->running && d->readingPlaying)
        frames += std::clamp(wallSeconds() - d->readingAt, 0.0, kMaxExtrapolationS) * kSampleRate;
    return frames;
}
