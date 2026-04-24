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

#ifndef PROBEMETRICS_H_DEFINED
#define PROBEMETRICS_H_DEFINED
#include <ProcessorHeaders.h>
#include <numeric>
#include <vector>

static constexpr int  FFT_SIZE            = 4096;
static constexpr int  FFT_BINS            = FFT_SIZE / 2 + 1;   // r2c output bins
static constexpr int  SNAPSHOT_WINDOW_MS  = 100;                // snapshot window duration

enum class ProbeStatus { UNKNOWN, PASS, WARN, FAIL };

struct ProbeMetrics
{
    // Identity
    String      streamName;
    String      serialNumber;
    int         numChannels  = 0;
    float       sampleRate   = 30000.0f;
    ProbeStatus status       = ProbeStatus::UNKNOWN;

    // Operator-adjustable thresholds
    float rmsThresholdUV     = 20.0f;
    float spikeRateFailHz    = 0.1f;
    float spikeRateLowHz     = 2.0f;
    float powerlineHz        = 60.0f;
    float powerlineSNRThresh = 10.0f;   // dB above median → noisy

    // Per-channel metrics (UI-thread copy)
    std::vector<float> rmsUV;              // [numChannels]
    std::vector<float> spikeRateHz;        // [numChannels] cumulative time-average
    std::vector<float> spikeRateLiveHz;    // [numChannels] most-recent window rate
    std::vector<float> powerSpectrum;      // [numChannels * FFT_BINS], linear power
    int                snapshotSamples = 3000; // = sampleRate * SNAPSHOT_WINDOW_MS / 1000
    std::vector<float> dataSnapshot;       // [numChannels * snapshotSamples], µV

    // RMS time-series heatmap: one frame per RMS window (~200 ms), durationSec * 5 total frames
    std::vector<float> rmsHistory;           // [rmsHistoryMaxFrames * numChannels]
    int rmsHistoryFrames    = 0;             // frames accumulated so far

    // Spike-rate time-series heatmap: live rate per RMS window, same cadence as rmsHistory
    std::vector<float> spikeRateHistory;     // [rmsHistoryMaxFrames * numChannels], Hz
    int spikeRateHistoryFrames = 0;
    int rmsWindowSamples     = 6000;    // 200 ms worth of samples at this stream's sample rate
    int rmsHistoryMaxFrames = 150;      // = durationSec * (sampleRate / rmsWindowSamples)
    int analysisDurationSec = 30;       // for X-axis labelling in the canvas

    // Acquisition state
    bool processingDone = false;        // true once the full duration has elapsed

    // Alert summaries 
    int numHighRmsChannels   = 0;
    int numLowSpikeChannels  = 0;
    int numNoisyChannels     = 0;

    // Per-channel band power strips (computed by processor, copied to canvas)
    std::vector<float> channelPowerlineDb;  // [numChannels] mean power ±3 bins around powerline fundamental (dB)
    std::vector<float> channelHFNoiseDb;    // [numChannels] mean power in 8–15 kHz band (dB)

    // Depth-sort mapping: channelOrder[displayRow] = original channel number (0-based within probe).
    // Identity when no depth metadata is available (i.e. unsorted).
    std::vector<int>   channelOrder;        // [numChannels]

    void allocate (int nCh, float sr, int durationSec = 30)
    {
        numChannels         = nCh;
        sampleRate          = sr;
        analysisDurationSec = durationSec;
        rmsWindowSamples    = std::max (1, int (sr * 0.2f)); // 200 ms at this stream's sample rate
        snapshotSamples     = std::max (1, int (sr * SNAPSHOT_WINDOW_MS / 1000.0f));
        const int maxFrames = std::max (1, int (float (durationSec) * sr / float (rmsWindowSamples)));
        rmsHistoryMaxFrames = maxFrames;
        rmsHistoryFrames         = 0;
        spikeRateHistoryFrames   = 0;
        processingDone           = false;
        rmsUV.assign                 (nCh, 0.0f);
        spikeRateHz.assign           (nCh, 0.0f);
        spikeRateLiveHz.assign       (nCh, 0.0f);
        powerSpectrum.assign         (nCh * FFT_BINS, 0.0f);
        dataSnapshot.assign          (nCh * snapshotSamples, 0.0f);
        rmsHistory.assign            (nCh * maxFrames, 0.0f);
        spikeRateHistory.assign      (nCh * maxFrames, 0.0f);
        channelPowerlineDb.assign    (nCh, 0.0f);
        channelHFNoiseDb.assign      (nCh, 0.0f);
        channelOrder.resize          (nCh);
        std::iota (channelOrder.begin(), channelOrder.end(), 0); // identity by default
    }

    void recomputeStatus()
    {
        if (numHighRmsChannels > 50 || numLowSpikeChannels > 50)
            status = ProbeStatus::FAIL;
        else if (numHighRmsChannels > 10 || numLowSpikeChannels > 10
                 || numNoisyChannels > 2)
            status = ProbeStatus::WARN;
        else
            status = ProbeStatus::PASS;
    }
};

#endif // PROBEMETRICS_H_DEFINED