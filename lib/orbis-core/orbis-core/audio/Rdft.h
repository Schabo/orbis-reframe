// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

// Internal to the STFT noise filters (WindReducer, GeneralWindReducer,
// HissHumReducer, VoiceIsolator):
// FFmpeg's av_tx real FFT and a sqrt-Hann window.

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#include <cmath>
#include <numbers>
#include <vector>

namespace insv {

inline std::vector<float> sqrtHann(int n)
{
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n)));
    return w;
}

// Real FFT on av_malloc'd (SIMD-aligned) scratch: av_tx needs the alignment.
struct Rdft {
    AVTXContext* fwd = nullptr;
    AVTXContext* inv = nullptr;
    av_tx_fn fwdFn = nullptr;
    av_tx_fn invFn = nullptr;
    int n = 0;
    float* real = nullptr;
    AVComplexFloat* cplx = nullptr;

    explicit Rdft(int size, bool withInverse) : n(size)
    {
        float s = 1.0f;
        av_tx_init(&fwd, &fwdFn, AV_TX_FLOAT_RDFT, 0, n, &s, 0);
        if (withInverse) {
            float si = 1.0f / static_cast<float>(n);
            av_tx_init(&inv, &invFn, AV_TX_FLOAT_RDFT, 1, n, &si, 0);
        }
        real = static_cast<float*>(av_malloc(sizeof(float) * (n + 2)));
        cplx = static_cast<AVComplexFloat*>(av_malloc(sizeof(AVComplexFloat) * (n / 2 + 2)));
    }
    ~Rdft()
    {
        av_tx_uninit(&fwd);
        av_tx_uninit(&inv);
        av_free(real);
        av_free(cplx);
    }
    Rdft(const Rdft&) = delete;
    Rdft& operator=(const Rdft&) = delete;

    bool ok() const { return fwdFn && real && cplx; }
    // real[0..n) -> cplx[0..n/2]
    void forward() { fwdFn(fwd, cplx, real, sizeof(float)); }
    // cplx[0..n/2] -> real[0..n) (scaled by 1/n)
    void inverse() { invFn(inv, real, cplx, sizeof(AVComplexFloat)); }
};

} // namespace insv
