/*
	------------------------------------------------------------------

	This file is part of the Open Ephys GUI
	Copyright (C) 2025 Open Ephys

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

//This prevents include loops. We recommend changing the macro to a name suitable for your plugin
#ifndef QUALITYMONITOR_H_DEFINED
#define QUALITYMONITOR_H_DEFINED

#include "ProbeMetrics.h"
#include <ProcessorHeaders.h>

#include <fftw3.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace QualityMonitorParams
{
inline constexpr auto kMaskedChannelsParam = "selected_channels";
inline constexpr auto kPowerlineHzParam = "powerline_hz";
inline constexpr auto kPowerlineSNRThreshParam = "powerline_snr_thresh_db";
inline constexpr auto kSpectrumFailChannelPercentageParam = "spectrum_fail_channel_percentage";
inline constexpr auto kRmsThresholdParam = "rms_threshold_uv";
inline constexpr auto kRmsFailChannelPercentageParam = "rms_fail_channel_percentage";
inline constexpr auto kSnapshotSaturationThresholdParam = "snapshot_saturation_threshold_uv";
inline constexpr auto kSnapshotFailChannelPercentageParam = "snapshot_fail_channel_percentage";
inline constexpr auto kSpikeFailHzParam = "spike_fail_hz";
inline constexpr auto kSpikeFailChannelPercentageParam = "spike_fail_channel_percentage";
inline constexpr auto kSyncMatchingDeviceThresholdsParam = "sync_matching_device_thresholds";
} // namespace QualityMonitorParams

// -- RAII wrapper: multi-channel batched r2c FFTW plan (O4) ---------------------------------------------------
// Owns SIMD-aligned input/output buffers and a fftw_plan_many_dft_r2c plan that
// processes all nCh channels in a single fftw_execute() call.
// Input layout (channel-major): channel c at inBuf + c * fftSize.
// Output layout (channel-major): channel c at outBuf + c * numBins.
class FFTProcessor
{
public:
    FFTProcessor (int n, int numChannels)
        : fftSize (n), numBins (n / 2 + 1), nCh (numChannels)
    {
        inBuf  = fftw_alloc_real    (size_t (fftSize) * nCh);
        outBuf = fftw_alloc_complex (size_t (numBins)  * nCh);
        // Single plan that batches all nCh channels; FFTW can exploit inter-channel SIMD.
        plan = fftw_plan_many_dft_r2c (
            1,        // rank
            &fftSize, // n
            nCh,      // howmany
            inBuf,  nullptr, 1, fftSize,  // in, inembed, istride, idist
            outBuf, nullptr, 1, numBins,  // out, onembed, ostride, odist
            FFTW_ESTIMATE);
        window.resize (fftSize);
        for (int k = 0; k < fftSize; ++k)
            window[k] = 0.5 * (1.0 - std::cos (2.0 * MathConstants<double>::pi * k / (fftSize - 1)));
    }

    ~FFTProcessor()
    {
        if (plan)   fftw_destroy_plan (plan);
        if (inBuf)  fftw_free (inBuf);
        if (outBuf) fftw_free (outBuf);
    }

    // Apply Hanning window to every channel in ringBuffer (channel-major,
    // channel c at ringBuffer + c * fftSize) and run the batched plan.
    void execute (const float* ringBuffer)
    {
        for (int c = 0; c < nCh; ++c)
        {
            const float* src = ringBuffer + c * fftSize;
            double*      dst = inBuf      + c * fftSize;
            for (int k = 0; k < fftSize; ++k)
                dst[k] = double (src[k]) * window[k];
        }
        fftw_execute (plan);
    }

    // Add |X[c][k]|^2 for all channels into accum[c * numBins + k].
    // The caller owns and manages accum; this method does NOT clear it.
    void accumulatePower (double* accum) const
    {
        for (int c = 0; c < nCh; ++c)
        {
            const fftw_complex* row  = outBuf + c * numBins;
            double*             dest = accum  + c * numBins;
            for (int k = 0; k < numBins; ++k)
            {
                const double re = row[k][0];
                const double im = row[k][1];
                dest[k] += re * re + im * im;
            }
        }
    }

    int getFFTSize() const { return fftSize; }
    int getNumBins() const { return numBins; }

    FFTProcessor (const FFTProcessor&) = delete;
    FFTProcessor& operator= (const FFTProcessor&) = delete;

private:
    int            fftSize;
    int            numBins;
    int            nCh;
    double*        inBuf  = nullptr;
    fftw_complex*  outBuf = nullptr;
    fftw_plan      plan   = nullptr;
    std::vector<double> window;
};

// -- Per-probe audio-thread state -----------------------------------------------
struct ProbeProcessingState
{
    // RMS
    std::vector<double> rmsSumSq;
    int rmsSampleCount = 0;

    // FFT ring buffer [numChannels * FFT_SIZE] and one shared FFTProcessor
    std::unique_ptr<FFTProcessor> fft;
    std::vector<float> fftRing;  // [numChannels * FFT_SIZE] ping-pong buffer 0
    std::vector<float> fftRingB; // [numChannels * FFT_SIZE] ping-pong buffer 1
    int activeFftBuf = 0;        // 0 = fftRing is active, 1 = fftRingB (audio-thread-only)
    int fftRingPos = 0;
    int fftWinCount = 0;         // legacy field; worker now uses FFTWorker::winCount
    std::vector<double> powerAccum; // [numChannels * FFT_BINS] worker-owned after O2

    // Spike detection
    std::vector<float> spikeThreshV; // adaptive 5× RMS, in raw V
    std::vector<bool> wasBelowThresh;
    std::vector<int> spikeCount; // window spikes (reset each 200 ms window)
    int spikeSampleCount = 0;
    std::vector<int64_t> cumSpikeCount; // run-total spikes per channel since acquisition start
    int64_t cumSpikeSamples = 0; // run-total samples counted for spikes
    bool spikeWarmupDone = false; // skip first window (uncalibrated threshold)

    // Snapshot ring buffer — audio-thread only, no lock needed
    std::vector<float> snapshotRing; // [numChannels * snapshotSamples]
    std::vector<uint8_t> snapshotSaturated; // [numChannels] latched once |signal| exceeds the snapshot saturation threshold
    int snapshotSamples = 3000; // = sampleRate * SNAPSHOT_WINDOW_MS / 1000
    int snapshotPos = 0; // next write position (wraps around)

    // Duration tracking (audio-thread only, no lock needed)
    int rmsWindowSamples = 6000; // 200 ms at this stream's sample rate
    int64_t totalSamplesAllowed = 0; // 0 = unlimited
    int64_t totalSamplesProcessed = 0;
    bool processingDone = false;

    // Pre-allocated scratch buffers — avoids heap allocation on the audio thread
    std::vector<float> scratchRms; // size nCh
    std::vector<float> scratchLiveRates; // size nCh
    std::vector<float> scratchLocalRates; // size nCh
    std::vector<float> scratchSpec; // size nCh * FFT_BINS
    std::vector<float> scratchSurround; // size 40 (±20 bin window)
    std::vector<float> scratchPlDb; // size nCh — per-channel powerline band power (dB)
    std::vector<float> scratchHFDb; // size nCh — per-channel 8–15 kHz mean power (dB)
    std::vector<float> scratchSnapshot; // size nCh * snapshotSamples — linearised ring, pre-built before lock

    /** Initialises all per-probe state for a new acquisition run.*/
    void allocate (int nCh, int windowSamples, int snapSamples);
};

/** 
	A plugin that includes a canvas for displaying incoming data
	or an extended settings interface.
*/

class QualityMonitor : public GenericProcessor
{
public:
    /** The class constructor, used to initialize any members.*/
    QualityMonitor();

    /** The class destructor, used to deallocate memory*/
    ~QualityMonitor();

    /** If the processor has a custom editor, this method must be defined to instantiate it. */
    AudioProcessorEditor* createEditor() override;

    /** Any parameter objects must be created inside this method */
    void registerParameters() override;

    /** Called when a processor parameter value is updated. */
    void parameterValueChanged (Parameter* parameter) override;

    /** Called every time the settings of an upstream plugin are changed.
		Allows the processor to handle variations in the channel configuration or any other parameter
		passed through signal chain. The processor can use this function to modify channel objects that
		will be passed to downstream plugins. */
    void updateSettings() override;

    /** Defines the functionality of the processor.
		The process method is called every time a new data buffer is available.
		Visualizer plugins typically use this method to send data to the canvas for display purposes */
    void process (AudioBuffer<float>& buffer) override;

    /** Thread-safe snapshot for the canvas timer. */
    void copyMetricsTo (Array<ProbeMetrics>& dest);

    /** Called from UI thread — brief lock, safe to call frequently. */
    void setRmsThreshold (int probeIdx, float uvThresh);
    void setRmsFailChannelPercentage (int probeIdx, float percentage);
    void setSpikeRateThreshold (int probeIdx, float failHz);
    void setSpikeFailChannelPercentage (int probeIdx, float percentage);
    void setPowerlineSNRThreshold (int probeIdx, float snrThreshDb);
    void setSpectrumFailChannelPercentage (int probeIdx, float percentage);
    void setSnapshotSaturationThreshold (int probeIdx, float uvThresh);
    void setSnapshotFailChannelPercentage (int probeIdx, float percentage);
    void setPowerlineHz (float hz);

    /** Returns the stream ID backing a probe sidebar entry. */
    uint16 getProbeStreamId (int probeIdx) const;

    /** Set the analysis duration (read at next startAcquisition). */
    void setDurationSeconds (int sec);

    /** Resets per-probe counters and history when acquisition starts.
        If autoStart is ON, processing begins immediately; otherwise it waits
        for a manual startProcessing() call. */
    bool startAcquisition() override;

    /** Called when acquisition stops. */
    bool stopAcquisition() override;

    /** Manually begin analysis (no-op if acquisition is not running). */
    void startProcessing();

    /** Abort an in-progress analysis run. */
    void stopProcessing();

    /** Enable or disable automatic processing start on acquisition. */
    void setAutoStart (bool enabled);

    /** Enable or disable copying threshold edits to streams with the same device name. */
    void setSyncMatchingDeviceThresholds (bool enabled);

    /** Copies the selected probe's threshold settings to streams with the same device name. */
    void syncThresholdsToMatchingDeviceStreams (int probeIdx);

    /** Returns whether threshold edits are copied to streams with the same device name. */
    bool getSyncMatchingDeviceThresholds() const { return syncMatchingDeviceThresholds.load(); }

    /** True while analysis is running (between startProcessing and allDone). */
    bool isProcessingActive() const { return processingHasStarted.load(); }

    /** Monotonically incremented (under metricsMutex) whenever probeMetrics is
        updated.  The canvas compares this against its own cached value to skip
        the expensive deep-copy when no new data has arrived since the last refresh. */
    uint32_t getMetricsGeneration() const noexcept { return metricsGeneration.load(); }

private:
    Array<ProbeMetrics> probeMetrics; // written audio / read UI
    std::vector<ProbeProcessingState> procState; // audio-thread only
    std::mutex metricsMutex;
    std::atomic<int> durationSeconds { 30 };
    std::atomic<bool> autoStartProcessing { true };
    std::atomic<bool> syncMatchingDeviceThresholds { false };
    std::atomic<bool> processingHasStarted { false };
    std::atomic<uint32_t> metricsGeneration { 0 }; // incremented under metricsMutex on every probeMetrics write
    bool applyingMatchedDeviceThresholds = false;

    std::vector<std::vector<int>> probeChannelIndices; // global buffer indices of DATA channels per stream
    std::vector<uint16> probeStreamIds; // stream ID for each probe (for per-stream sample count)
    int totalProbes = 0;

    // ── Per-probe FFT worker (O2) ──────────────────────────────────────────────
    // Stored as unique_ptr so that std::mutex / std::condition_variable /
    // std::thread stay at stable heap addresses even if procState is reallocated.
    struct FFTWorker
    {
        std::thread             thread;
        std::mutex              mutex;
        std::condition_variable cv;
        std::atomic<int>        workBuf { -1 };  // -1 = idle; 0/1 = ring index ready for FFT
        std::atomic<bool>       stop    { false };
        int                     winCount = 0;    // windows accumulated since last finalizeFFT
    };
    std::vector<std::unique_ptr<FFTWorker>> fftWorkers;

    /** Stop the worker for probe pi and join its thread (message-thread safe). */
    void stopFftWorker (int pi);
    /** Allocate and start a new FFT worker for probe pi. Must be called after
        procState[pi] is fully initialised and processingHasStarted is false. */
    void startFftWorker (int pi);

    void finalizeRms (int pi);
    void finalizeFFT (int pi);
    void finalizeSpikes (int pi);
    void captureSnapshot (int pi, AudioBuffer<float>& buf);

    void applyThresholdToMatchingDeviceStreams (uint16 sourceStreamId, const String& parameterName, float value);

    /** Shared reset + start logic used by both startAcquisition and startProcessing. */
    void doStartProcessing();

    /** Generates an assertion if this class leaks */
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QualityMonitor);
};

#endif // QUALITYMONITOR_H_DEFINED
