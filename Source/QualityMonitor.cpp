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

#include "QualityMonitor.h"

#include "QualityMonitorEditor.h"

QualityMonitor::QualityMonitor()
    : GenericProcessor ("Quality Monitor")
{
}

QualityMonitor::~QualityMonitor()
{
}

AudioProcessorEditor* QualityMonitor::createEditor()
{
    editor = std::make_unique<QualityMonitorEditor> (this);
    return editor.get();
}

void QualityMonitor::registerParameters()
{
    // Register any parameters here if needed
    // For example: addSelectedChannelsParameter (Parameter::STREAM_SCOPE,
    //                                 channels,
    //                                 Channels,
    //                                 Selected channels to use);
}

void QualityMonitor::updateSettings()
{
    // Enumerate all data streams; keep only those with at least one ELECTRODE (DATA) channel
    Array<const DataStream*> dataStreams = getDataStreams();

    Array<const DataStream*>     validStreams;
    std::vector<std::vector<int>> channelIndicesPerStream;

    int globalOff = 0;
    for (auto* stream : dataStreams)
    {
        std::vector<int> dataIndices;
        const Array<ContinuousChannel*> channels = stream->getContinuousChannels();
        for (int localIdx = 0; localIdx < channels.size(); ++localIdx)
        {
            if (channels[localIdx]->getChannelType() == ContinuousChannel::ELECTRODE)
                dataIndices.push_back (globalOff + localIdx);
        }

        if (! dataIndices.empty())
        {
            validStreams.add (stream);
            channelIndicesPerStream.push_back (std::move (dataIndices));
        }

        globalOff += stream->getChannelCount();
    }

    totalProbes = validStreams.size();
    if (totalProbes == 0)
        return;

    probeChannelIndices.resize (totalProbes);
    probeStreamIds.resize (totalProbes);
    for (int pi = 0; pi < totalProbes; ++pi)
    {
        probeChannelIndices[pi] = std::move (channelIndicesPerStream[pi]);
        probeStreamIds[pi] = validStreams[pi]->getStreamId();
    }

    // Allocate metrics and processing state
    {
        std::lock_guard<std::mutex> lock (metricsMutex);
        probeMetrics.resize (totalProbes);
        for (int pi = 0; pi < totalProbes; ++pi)
        {
            const float sr  = validStreams[pi]->getSampleRate();
            const int   nCh = (int) probeChannelIndices[pi].size();
            const int   dur = durationSeconds.load();

            // Preserve operator thresholds across reconfiguration
            const float prevRmsThr = probeMetrics.getReference(pi).rmsThresholdUV > 0.0f ? probeMetrics.getReference(pi).rmsThresholdUV : 20.0f;
            const float prevPlHz = probeMetrics.getReference(pi).powerlineHz > 0.0f ? probeMetrics.getReference(pi).powerlineHz : 60.0f;

            probeMetrics.getReference(pi).allocate (nCh, sr, dur);
            probeMetrics.getReference(pi).streamName = validStreams[pi]->getName();
            probeMetrics.getReference(pi).rmsThresholdUV = prevRmsThr;
            probeMetrics.getReference(pi).powerlineHz = prevPlHz;
        }
    }

    // ProbeProcessingState owns FFTProcessor (RAII) — safe to reconstruct
    procState.resize (totalProbes);
    for (int pi = 0; pi < totalProbes; ++pi)
    {
        const int   nCh = (int) probeChannelIndices[pi].size();
        const float sr  = validStreams[pi]->getSampleRate();
        const int   dur = durationSeconds.load();
        procState[pi].allocate (nCh, probeMetrics.getReference(pi).rmsWindowSamples);
        procState[pi].totalSamplesAllowed = int64_t (dur) * int64_t (sr + 0.5f);
    }
}

void QualityMonitor::process (AudioBuffer<float>& buffer)
{
    const int totalCh = buffer.getNumChannels();

    for (int pi = 0; pi < totalProbes; ++pi)
    {
        const int numSamples = getNumSamplesInBlock (probeStreamIds[pi]);
        const auto& chIndices = probeChannelIndices[pi];
        const int nCh = (int) chIndices.size();
        if (nCh == 0 || chIndices.back() >= totalCh)
            continue;

        auto& ps = procState[pi];

        // Stop processing once the full duration has elapsed
        if (ps.processingDone)
            continue;

        // 1 & 3. RMS + spike accumulation in rmsWindowSamples-sized chunks so
        // that each finalized frame covers exactly rmsWindowSamples samples
        // regardless of the audio-callback buffer size.  Without chunking, a
        // large buffer (e.g. 10 000 samples) generates only one frame per call
        // instead of the expected 10000/6000 ≈ 1.67, causing the heatmap to
        // appear to stop at ~60 % (18 s) of a 30 s window.
        {
            int rmsOffset = 0;
            int rmsRemain = numSamples;

            while (rmsRemain > 0)
            {
                const int space = ps.rmsWindowSamples - ps.rmsSampleCount;
                const int chunk = std::min (rmsRemain, space);

                // RMS accumulation for this chunk
                for (int c = 0; c < nCh; ++c)
                {
                    const float* src = buffer.getReadPointer (chIndices[c]) + rmsOffset;
                    double sq = 0.0;
                    for (int i = 0; i < chunk; ++i)
                        sq += double (src[i]) * double (src[i]);
                    ps.rmsSumSq[c] += sq;
                }

                // Spike detection for this chunk
                for (int c = 0; c < nCh; ++c)
                {
                    const float* src = buffer.getReadPointer (chIndices[c]) + rmsOffset;
                    const float thr = ps.spikeThreshV[c];
                    for (int i = 0; i < chunk; ++i)
                    {
                        const bool below = src[i] < -thr;
                        if (below && ! ps.wasBelowThresh[c])
                            ps.spikeCount[c]++;
                        ps.wasBelowThresh[c] = below;
                    }
                }

                ps.rmsSampleCount   += chunk;
                ps.spikeSampleCount += chunk;
                rmsOffset           += chunk;
                rmsRemain           -= chunk;

                if (ps.rmsSampleCount >= ps.rmsWindowSamples)
                {
                    finalizeRms    (pi);
                    finalizeSpikes (pi);
                }
            }
        }

        // 2. FFT ring-buffer accumulation
        {
            const int fftSize = ps.fft->getFFTSize();
            int srcOffset = 0;
            int remain = numSamples;

            while (remain > 0)
            {
                const int chunk = std::min (remain, fftSize - ps.fftRingPos);

                // Copy 'chunk' samples from each channel into their ring slot
                for (int c = 0; c < nCh; ++c)
                {
                    const float* src = buffer.getReadPointer (chIndices[c]);
                    float* ring = ps.fftRing.data() + c * fftSize + ps.fftRingPos;
                    std::copy (src + srcOffset, src + srcOffset + chunk, ring);
                }

                ps.fftRingPos += chunk;
                srcOffset += chunk;
                remain -= chunk;

                if (ps.fftRingPos == fftSize)
                {
                    // Run FFT for each channel and accumulate power
                    for (int c = 0; c < nCh; ++c)
                    {
                        ps.fft->execute (ps.fftRing.data() + c * fftSize);
                        double* acc = ps.powerAccum.data() + c * FFT_BINS;
                        for (int k = 0; k < FFT_BINS; ++k)
                            acc[k] += ps.fft->powerAt (k);
                    }
                    ps.fftRingPos = 0;
                    ps.fftWinCount++;
                }
            }
        }

        // 3. Raw voltage snapshot (cheapest: overwrite every buffer)
        captureSnapshot (pi, buffer);

        // 4. Finalize FFT when enough windows have accumulated
        if (ps.fftWinCount >= 8)
            finalizeFFT (pi);

        // 5. Track total samples; end processing when duration elapses
        ps.totalSamplesProcessed += numSamples;
        if (ps.totalSamplesAllowed > 0 &&
            ps.totalSamplesProcessed >= ps.totalSamplesAllowed)
        {
            // Force-finalize any partial windows
            if (ps.rmsSampleCount > 0) { finalizeRms (pi); finalizeSpikes (pi); }
            if (ps.fftWinCount > 0)    finalizeFFT (pi);

            ps.processingDone = true;
            std::lock_guard<std::mutex> lock (metricsMutex);
            probeMetrics.getReference (pi).processingDone = true;
        }
    }
}

void QualityMonitor::finalizeRms (int pi)
{
    auto& ps = procState[pi];
    const int nCh = (int) probeChannelIndices[pi].size();
    const int n = ps.rmsSampleCount;
    if (n == 0)
        return;

    std::vector<float> localRms (nCh);
    for (int c = 0; c < nCh; ++c)
    {
        const float rmsV = float (std::sqrt (ps.rmsSumSq[c] / n));
        localRms[c] = rmsV * 1.0e6f; // V → µV for display
        ps.spikeThreshV[c] = std::max (rmsV * 5.0f, 1e-7f); // adaptive spike thr
        ps.rmsSumSq[c] = 0.0;
    }
    ps.rmsSampleCount = 0;

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.rmsUV = localRms;
    m.numHighRmsChannels = 0;
    for (float v : m.rmsUV)
        if (v > m.rmsThresholdUV)
            m.numHighRmsChannels++;

    // Append this frame to the rolling RMS history heatmap
    if (m.rmsHistoryFrames < m.rmsHistoryMaxFrames)
    {
        const int frameOff = m.rmsHistoryFrames * nCh;
        for (int c = 0; c < nCh; ++c)
            m.rmsHistory[frameOff + c] = localRms[c];
        m.rmsHistoryFrames++;
    }

    m.recomputeStatus();
}

void QualityMonitor::finalizeFFT (int pi)
{
    auto& ps = procState[pi];
    const int nCh = (int) probeChannelIndices[pi].size();
    const int nWin = std::max (1, ps.fftWinCount);

    // Average accumulated power and clear accumulators
    std::vector<float> localSpec (nCh * FFT_BINS);
    for (int c = 0; c < nCh; ++c)
    {
        double* acc = ps.powerAccum.data() + c * FFT_BINS;
        float* dest = localSpec.data() + c * FFT_BINS;
        for (int k = 0; k < FFT_BINS; ++k)
        {
            dest[k] = float (acc[k] / nWin);
            acc[k] = 0.0;
        }
    }
    ps.fftWinCount = 0;

    // -- Noisy channel detection --
    // Snapshot these under no-lock (written only in updateSettings on message thread)
    const float sr = probeMetrics[pi].sampleRate;
    const float plHz = probeMetrics[pi].powerlineHz;
    const float snrThr = probeMetrics[pi].powerlineSNRThresh;

    const float hzPerBin = sr / float (FFT_SIZE);
    auto hzToBin = [&] (float hz) -> int
    {
        return jlimit (0, FFT_BINS - 1, int (hz / hzPerBin));
    };

    // Check primary powerline and harmonics up to Nyquist
    const float harmonics[] = { plHz, plHz * 2.0f, plHz * 3.0f, 50.0f, 100.0f, 150.0f };

    int numNoisyCh = 0;
    for (int c = 0; c < nCh; ++c)
    {
        const float* row = localSpec.data() + c * FFT_BINS;
        bool noisy = false;

        for (float h : harmonics)
        {
            const float nyquist = sr / 2.0f;
            if (h <= 0.0f || h >= nyquist)
                continue;

            const int bin = hzToBin (h);
            if (bin <= 0 || bin >= FFT_BINS - 1)
                continue;

            // Local noise floor: median of ±20 bins, excluding ±3 around peak
            std::vector<float> surround;
            surround.reserve (34);
            for (int kb = bin - 20; kb <= bin + 20; ++kb)
            {
                if (kb < 0 || kb >= FFT_BINS)
                    continue;
                if (std::abs (kb - bin) <= 3)
                    continue;
                surround.push_back (row[kb]);
            }
            if (surround.empty())
                continue;

            std::sort (surround.begin(), surround.end());
            const float med = surround[surround.size() / 2];
            if (med < 1e-30f)
                continue;

            const float snrDb = 10.0f * std::log10 (row[bin] / med);
            if (snrDb > snrThr)
            {
                noisy = true;
                break;
            }
        }
        if (noisy)
            numNoisyCh++;
    }

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.powerSpectrum = localSpec;
    m.numNoisyChannels = numNoisyCh;
    m.recomputeStatus();
}

void QualityMonitor::finalizeSpikes (int pi)
{
    auto& ps = procState[pi];
    const int nCh = (int) probeChannelIndices[pi].size();
    const float elapsedSec = std::max (
        float (ps.spikeSampleCount) / probeMetrics[pi].sampleRate, 0.001f);

    std::vector<float> localRates (nCh);
    for (int c = 0; c < nCh; ++c)
    {
        localRates[c] = float (ps.spikeCount[c]) / elapsedSec;
        ps.spikeCount[c] = 0;
    }
    ps.spikeSampleCount = 0;

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.spikeRateHz = localRates;
    m.numLowSpikeChannels = 0;
    for (float r : m.spikeRateHz)
        if (r < m.spikeRateFailHz)
            m.numLowSpikeChannels++;
    m.recomputeStatus();
}

void QualityMonitor::captureSnapshot (int pi, AudioBuffer<float>& buffer)
{
    const auto& chIndices = probeChannelIndices[pi];
    const int nCh = (int) chIndices.size();
    const int N = std::min (buffer.getNumSamples(), SNAPSHOT_SAMPLES);

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    for (int c = 0; c < nCh; ++c)
    {
        const float* src = buffer.getReadPointer (chIndices[c]);
        float* dst = m.dataSnapshot.data() + c * SNAPSHOT_SAMPLES;
        for (int i = 0; i < N; ++i)
            dst[i] = src[i] * 1.0e6f; // V → µV
    }
}

void QualityMonitor::copyMetricsTo (Array<ProbeMetrics>& dest)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    dest = probeMetrics;
}

void QualityMonitor::setRmsThreshold (int pi, float uv)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
        probeMetrics.getReference (pi).rmsThresholdUV = uv;
}

void QualityMonitor::setSpikeRateThresh (int pi, float failHz, float lowHz)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        probeMetrics.getReference (pi).spikeRateFailHz = failHz;
        probeMetrics.getReference (pi).spikeRateLowHz = lowHz;
    }
}

void QualityMonitor::setPowerlineHz (int pi, float hz)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
        probeMetrics.getReference (pi).powerlineHz = hz;
}

void QualityMonitor::setDurationSeconds (int sec)
{
    durationSeconds.store (jlimit (1, 300, sec));
}

bool QualityMonitor::startAcquisition()
{
    const int   dur = durationSeconds.load();

    // Reset procState (safe: audio thread not running yet)
    for (int pi = 0; pi < totalProbes; ++pi)
    {
        auto& ps = procState[pi];
        ps.totalSamplesProcessed = 0;
        ps.processingDone        = false;
        ps.rmsSampleCount        = 0;
        std::fill (ps.rmsSumSq.begin(),    ps.rmsSumSq.end(),    0.0);
        std::fill (ps.spikeCount.begin(),  ps.spikeCount.end(),  0);
        ps.spikeSampleCount = 0;
        ps.fftRingPos       = 0;
        ps.fftWinCount      = 0;
        std::fill (ps.powerAccum.begin(), ps.powerAccum.end(), 0.0);
    }

    // Reset probeMetrics history (under lock)
    {
        std::lock_guard<std::mutex> lock (metricsMutex);
        for (int pi = 0; pi < totalProbes; ++pi)
        {
            auto& m = probeMetrics.getReference (pi);
            const float sr  = m.sampleRate;
            const int   nCh = m.numChannels;
            const int   maxFrames = std::max (1, int (float (dur) * sr / float (m.rmsWindowSamples)));

            m.processingDone      = false;
            m.rmsHistoryFrames    = 0;
            m.analysisDurationSec = dur;
            m.rmsHistoryMaxFrames = maxFrames;
            m.rmsHistory.assign   (nCh * maxFrames, 0.0f);

            procState[pi].totalSamplesAllowed =
                int64_t (dur) * int64_t (sr + 0.5f);
        }
    }

    return true;
}