/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef HIGHPASSFILTER_H_DEFINED
#define HIGHPASSFILTER_H_DEFINED

#include <cmath>

// ── SIMD detection ────────────────────────────────────────────────────────────
// Normalise MSVC /arch:AVX → SSE4.1 so the detection macro is consistent.
#if defined(_MSC_VER) && defined(__AVX__) && ! defined(__SSE4_1__)
#define __SSE4_1__ 1
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define QM_USE_NEON 1
#else
#define QM_USE_NEON 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#define QM_USE_SSE 1
#else
#define QM_USE_SSE 0
#endif

#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

// ── SIMD backend selection ────────────────────────────────────────────────────

enum class HighPassSIMDType
{
    None,
    SSE2,
    SSE4_1,
    NEON
};

/** Detects and caches the best available SIMD backend at runtime. */
inline HighPassSIMDType getAvailableHighPassSIMD()
{
    static bool cached = false;
    static HighPassSIMDType result = HighPassSIMDType::None;
    if (cached)
        return result;
    cached = true;

#if QM_USE_NEON
    result = HighPassSIMDType::NEON;
    return result;
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    int cpuInfo[4] = {};
#if defined(_MSC_VER)
    __cpuid (cpuInfo, 1);
#elif defined(__GNUC__) || defined(__clang__)
    __cpuid (1, cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);
#endif
    const bool hasSSE4_1 = (cpuInfo[2] & (1 << 19)) != 0;
    const bool hasSSE2 = (cpuInfo[3] & (1 << 26)) != 0;
#if defined(__SSE4_1__)
    if (hasSSE4_1)
    {
        result = HighPassSIMDType::SSE4_1;
        return result;
    }
#endif
#if QM_USE_SSE
    if (hasSSE2)
    {
        result = HighPassSIMDType::SSE2;
        return result;
    }
#endif
#endif

    result = HighPassSIMDType::None;
    return result;
}

// ── Coefficient computation ───────────────────────────────────────────────────

/**
    Computes RBJ cookbook 2nd-order high-pass biquad coefficients.
    Cutoff: 300 Hz, Q = 1/sqrt(2) (Butterworth maximally-flat response).
    Output coefficients are for the Transposed Direct Form II structure.
*/
inline void computeHighPassCoefficients (float sampleRate,
                                         float& b0,
                                         float& b1,
                                         float& b2,
                                         float& a1,
                                         float& a2)
{
    constexpr float cutoffHz = 300.0f;
    constexpr float Q = 0.70710678f; // 1/sqrt(2)
    constexpr float pi = 3.14159265358979323846f;

    const float w0 = 2.0f * pi * cutoffHz / sampleRate;
    const float cosW0 = std::cos (w0);
    const float sinW0 = std::sin (w0);
    const float alpha = sinW0 / (2.0f * Q);

    const float rb0 = (1.0f + cosW0) * 0.5f;
    const float rb1 = -(1.0f + cosW0);
    const float rb2 = (1.0f + cosW0) * 0.5f;
    const float a0 = 1.0f + alpha;
    const float ra1 = -2.0f * cosW0;
    const float ra2 = 1.0f - alpha;

    b0 = rb0 / a0;
    b1 = rb1 / a0;
    b2 = rb2 / a0;
    a1 = ra1 / a0;
    a2 = ra2 / a0;
}

// ── Scalar single-channel processing ─────────────────────────────────────────

/**
    Runs one channel through the TDF-II biquad, accumulating squared output
    into rmsSumSq and counting negative threshold crossings as spikes.
*/
inline void processChannelScalar (const float* src, int N, float b0, float b1, float b2, float a1, float a2, float& z1, float& z2, float thr, bool& prevBelow, int& spikes, double& rmsSumSq)
{
    for (int i = 0; i < N; ++i)
    {
        const float x = src[i];
        const float val = b0 * x + z1;
        z1 = b1 * x - a1 * val + z2;
        z2 = b2 * x - a2 * val;
        rmsSumSq += double (val) * double (val);
        const bool below = val < -thr;
        if (below && ! prevBelow)
            spikes++;
        prevBelow = below;
    }
}

// ── SSE lane-per-channel: 4 channels × N samples ─────────────────────────────

#if QM_USE_SSE
/**
    Processes 4 channels simultaneously using SSE, with one channel per SIMD lane.
    sqAcc[4] are float squared-sum accumulators for the chunk; the caller is
    responsible for adding each lane's value to its corresponding double rmsSumSq.
    Spike detection remains scalar (sequential prevBelow dependency).
*/
inline void processBlock4SSE (const float* d0, const float* d1, const float* d2, const float* d3, int N, float b0, float b1, float b2, float a1, float a2, float* z1arr, float* z2arr, const float* thr, bool* prevBelow, int* spikes, float* sqAcc)
{
    const __m128 vb0 = _mm_set1_ps (b0);
    const __m128 vb1 = _mm_set1_ps (b1);
    const __m128 vb2 = _mm_set1_ps (b2);
    const __m128 va1 = _mm_set1_ps (a1);
    const __m128 va2 = _mm_set1_ps (a2);

    // Load per-lane filter state (lane 0 = ch+0, lane 1 = ch+1, ...)
    __m128 z1 = _mm_set_ps (z1arr[3], z1arr[2], z1arr[1], z1arr[0]);
    __m128 z2 = _mm_set_ps (z2arr[3], z2arr[2], z2arr[1], z2arr[0]);
    __m128 sq = _mm_set_ps (sqAcc[3], sqAcc[2], sqAcc[1], sqAcc[0]);

    alignas (16) float y[4];

    for (int i = 0; i < N; ++i)
    {
        const __m128 x = _mm_set_ps (d3[i], d2[i], d1[i], d0[i]);

        // TDF-II biquad update
        const __m128 out = _mm_add_ps (_mm_mul_ps (vb0, x), z1);
        z1 = _mm_add_ps (_mm_sub_ps (_mm_mul_ps (vb1, x), _mm_mul_ps (va1, out)), z2);
        z2 = _mm_sub_ps (_mm_mul_ps (vb2, x), _mm_mul_ps (va2, out));
        sq = _mm_add_ps (sq, _mm_mul_ps (out, out));

        // Spike detection — sequential prevBelow dependency, remains scalar
        _mm_storeu_ps (y, out);
        for (int lane = 0; lane < 4; ++lane)
        {
            const bool below = y[lane] < -thr[lane];
            if (below && ! prevBelow[lane])
                spikes[lane]++;
            prevBelow[lane] = below;
        }
    }

    // Persist filter states (_mm_storeu_ps matches _mm_set_ps lane order)
    _mm_storeu_ps (z1arr, z1);
    _mm_storeu_ps (z2arr, z2);
    _mm_storeu_ps (sqAcc, sq);
}
#endif // QM_USE_SSE

// ── NEON lane-per-channel: 4 channels × N samples ────────────────────────────

#if QM_USE_NEON
/**
    Processes 4 channels simultaneously using ARM NEON, with one channel per
    SIMD lane. Interface mirrors processBlock4SSE.
*/
inline void processBlock4NEON (const float* d0, const float* d1, const float* d2, const float* d3, int N, float b0, float b1, float b2, float a1, float a2, float* z1arr, float* z2arr, const float* thr, bool* prevBelow, int* spikes, float* sqAcc)
{
    const float32x4_t vb0 = vdupq_n_f32 (b0);
    const float32x4_t vb1 = vdupq_n_f32 (b1);
    const float32x4_t vb2 = vdupq_n_f32 (b2);
    const float32x4_t va1 = vdupq_n_f32 (a1);
    const float32x4_t va2 = vdupq_n_f32 (a2);

    float32x4_t z1 = { z1arr[0], z1arr[1], z1arr[2], z1arr[3] };
    float32x4_t z2 = { z2arr[0], z2arr[1], z2arr[2], z2arr[3] };
    float32x4_t sq = { sqAcc[0], sqAcc[1], sqAcc[2], sqAcc[3] };

    alignas (16) float y[4];

    for (int i = 0; i < N; ++i)
    {
        const float32x4_t x = { d0[i], d1[i], d2[i], d3[i] };

        // vmlaq_f32(acc, a, b) = acc + a*b → out = b0*x + z1
        const float32x4_t out = vmlaq_f32 (z1, vb0, x);
        z1 = vaddq_f32 (vsubq_f32 (vmulq_f32 (vb1, x), vmulq_f32 (va1, out)), z2);
        z2 = vsubq_f32 (vmulq_f32 (vb2, x), vmulq_f32 (va2, out));
        sq = vmlaq_f32 (sq, out, out);

        // Spike detection — sequential prevBelow dependency, remains scalar
        vst1q_f32 (y, out);
        for (int lane = 0; lane < 4; ++lane)
        {
            const bool below = y[lane] < -thr[lane];
            if (below && ! prevBelow[lane])
                spikes[lane]++;
            prevBelow[lane] = below;
        }
    }

    vst1q_f32 (z1arr, z1);
    vst1q_f32 (z2arr, z2);
    vst1q_f32 (sqAcc, sq);
}
#endif // QM_USE_NEON

#endif // HIGHPASSFILTER_H_DEFINED
