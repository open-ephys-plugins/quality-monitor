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
    if (! processingHasStarted.load())
        return;

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

        // 3. Raw voltage snapshot: accumulate into audio-thread ring buffer
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
            {
                std::lock_guard<std::mutex> lock (metricsMutex);
                probeMetrics.getReference (pi).processingDone = true;
            }

            // If every probe is now done, clear the flag so a new Capture run
            // can be started without stopping/restarting acquisition.
            bool allDone = true;
            for (int i = 0; i < totalProbes; ++i)
                if (! procState[i].processingDone) { allDone = false; break; }
            if (allDone)
                processingHasStarted.store (false);
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
        const float rms = float (std::sqrt (ps.rmsSumSq[c] / n));
        localRms[c] = rms;
        ps.spikeThreshV[c] = std::max (rms * 5.0f, 1e-7f); // adaptive spike thr
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

    // Linearize snapshot ring buffer into probeMetrics (lock already held)
    {
        const int writePos = ps.snapshotPos;
        const int tail     = SNAPSHOT_SAMPLES - writePos;
        for (int c = 0; c < nCh; ++c)
        {
            const float* ring = ps.snapshotRing.data() + c * SNAPSHOT_SAMPLES;
            float*       dst  = m.dataSnapshot.data()  + c * SNAPSHOT_SAMPLES;
            std::copy (ring + writePos, ring + SNAPSHOT_SAMPLES, dst);
            std::copy (ring,            ring + writePos,         dst + tail);
        }
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

    // First window used bootstrap threshold — discard its counts so spikes
    // detected before the adaptive threshold was calibrated don't bias the average.
    if (!ps.spikeWarmupDone)
    {
        std::fill (ps.spikeCount.begin(), ps.spikeCount.end(), 0);
        ps.spikeSampleCount = 0;
        ps.spikeWarmupDone  = true;
        return;
    }

    // Compute per-window live rate BEFORE accumulating into cumulative totals
    const float windowSec = std::max (float (ps.spikeSampleCount) / probeMetrics[pi].sampleRate, 0.001f);
    std::vector<float> liveRates (nCh);
    for (int c = 0; c < nCh; ++c)
        liveRates[c] = float (ps.spikeCount[c]) / windowSec;

    // Accumulate window counts into run totals, then reset the window
    for (int c = 0; c < nCh; ++c)
    {
        ps.cumSpikeCount[c] += ps.spikeCount[c];
        ps.spikeCount[c]     = 0;
    }
    ps.cumSpikeSamples  += ps.spikeSampleCount;
    ps.spikeSampleCount  = 0;

    // Average rate over the entire run so far
    const float totalElapsedSec = std::max (
        float (ps.cumSpikeSamples) / probeMetrics[pi].sampleRate, 0.001f);

    std::vector<float> localRates (nCh);
    for (int c = 0; c < nCh; ++c)
        localRates[c] = float (ps.cumSpikeCount[c]) / totalElapsedSec;

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.spikeRateHz     = localRates;
    m.spikeRateLiveHz = liveRates;
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
    const int numSamples = getNumSamplesInBlock (probeStreamIds[pi]);
    auto& ps = procState[pi];
    const int N = std::min (numSamples, SNAPSHOT_SAMPLES);

    // Write into the ring buffer (audio-thread only — no lock needed)
    for (int s = 0; s < N; ++s)
    {
        const int dst = ps.snapshotPos;
        for (int c = 0; c < nCh; ++c)
            ps.snapshotRing[c * SNAPSHOT_SAMPLES + dst] =
                buffer.getReadPointer (chIndices[c])[s];
        ps.snapshotPos = (ps.snapshotPos + 1) % SNAPSHOT_SAMPLES;
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
    processingHasStarted.store (false);

    if (autoStartProcessing.load())
        doStartProcessing();

    return true;
}

bool QualityMonitor::stopAcquisition()
{
    processingHasStarted.store (false);
    return true;
}

void QualityMonitor::startProcessing()
{
    if (! CoreServices::getAcquisitionStatus() && ! processingHasStarted.load())
        return;
    doStartProcessing();
}

void QualityMonitor::setAutoStart (bool enabled)
{
    autoStartProcessing.store (enabled);
}

void QualityMonitor::doStartProcessing()
{
    const int dur = durationSeconds.load();

    // Reset procState — safe because the audio thread either has not started
    // yet (startAcquisition path) or processingHasStarted is still false so
    // process() returns immediately without touching procState.
    for (int pi = 0; pi < totalProbes; ++pi)
    {
        auto& ps = procState[pi];
        ps.totalSamplesProcessed = 0;
        ps.processingDone        = false;
        ps.rmsSampleCount        = 0;
        std::fill (ps.rmsSumSq.begin(),      ps.rmsSumSq.end(),      0.0);
        std::fill (ps.spikeCount.begin(),    ps.spikeCount.end(),    0);
        ps.spikeSampleCount = 0;
        std::fill (ps.cumSpikeCount.begin(), ps.cumSpikeCount.end(), 0LL);
        ps.cumSpikeSamples  = 0;
        ps.spikeWarmupDone  = false;
        ps.fftRingPos       = 0;
        ps.fftWinCount      = 0;
        std::fill (ps.powerAccum.begin(),    ps.powerAccum.end(),    0.0);
        std::fill (ps.snapshotRing.begin(),  ps.snapshotRing.end(),  0.0f);
        ps.snapshotPos = 0;
    }

    {
        std::lock_guard<std::mutex> lock (metricsMutex);
        for (int pi = 0; pi < totalProbes; ++pi)
        {
            auto& m = probeMetrics.getReference (pi);
            const float sr        = m.sampleRate;
            const int   nCh       = m.numChannels;
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

    processingHasStarted.store (true);
}