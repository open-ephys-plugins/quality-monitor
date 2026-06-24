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
#include <cstdint>
#include <memory>
#include <mutex>
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

// -- RAII wrapper: owns one r2c FFTW plan + its aligned buffers -------------------------------------------------
class FFTProcessor
{
public:
    explicit FFTProcessor (int n)
        : fftSize (n), numBins (n / 2 + 1)
    {
        // fftw_alloc_real / fftw_alloc_complex guarantee SIMD alignment
        inBuf = fftw_alloc_real (fftSize);
        outBuf = fftw_alloc_complex (numBins);
        // FFTW_ESTIMATE avoids overwriting buffers during planning
        plan = fftw_plan_dft_r2c_1d (fftSize, inBuf, outBuf, FFTW_ESTIMATE);
        // Precompute Hanning window
        window.resize (fftSize);
        for (int k = 0; k < fftSize; ++k)
            window[k] = 0.5 * (1.0 - std::cos (2.0 * MathConstants<double>::pi * k / (fftSize - 1)));
    }

    ~FFTProcessor()
    {
        if (plan)
            fftw_destroy_plan (plan);
        if (inBuf)
            fftw_free (inBuf);
        if (outBuf)
            fftw_free (outBuf);
    }

    // Execute r2c on 'src' (float, length fftSize). Fills outBuf.
    void execute (const float* src)
    {
        for (int k = 0; k < fftSize; ++k)
            inBuf[k] = src[k] * window[k];
        fftw_execute (plan);
    }

    // Power at bin k = |X[k]|^2
    double powerAt (int k) const
    {
        double re = outBuf[k][0];
        double im = outBuf[k][1];
        return re * re + im * im;
    }

    int getFFTSize() const { return fftSize; }
    int getNumBins() const { return numBins; }

    // Non-copyable, non-movable (plan holds raw pointers)
    FFTProcessor (const FFTProcessor&) = delete;
    FFTProcessor& operator= (const FFTProcessor&) = delete;

private:
    int fftSize;
    int numBins;
    double* inBuf = nullptr;
    fftw_complex* outBuf = nullptr;
    fftw_plan plan = nullptr;
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
    std::vector<float> fftRing; // [numChannels * FFT_SIZE]
    int fftRingPos = 0;
    int fftWinCount = 0;
    std::vector<double> powerAccum; // [numChannels * FFT_BINS]

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

private:
    Array<ProbeMetrics> probeMetrics; // written audio / read UI
    std::vector<ProbeProcessingState> procState; // audio-thread only
    std::mutex metricsMutex;
    std::atomic<int> durationSeconds { 30 };
    std::atomic<bool> autoStartProcessing { true };
    std::atomic<bool> syncMatchingDeviceThresholds { false };
    std::atomic<bool> processingHasStarted { false };
    bool applyingMatchedDeviceThresholds = false;

    std::vector<std::vector<int>> probeChannelIndices; // global buffer indices of DATA channels per stream
    std::vector<uint16> probeStreamIds; // stream ID for each probe (for per-stream sample count)
    int totalProbes = 0;

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
