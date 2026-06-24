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

#include <algorithm>
#include <numeric>

namespace
{
using namespace QualityMonitorParams;

float getFloatParameterValue (Parameter* parameter, float fallback)
{
    if (auto* floatParameter = dynamic_cast<FloatParameter*> (parameter))
        return floatParameter->getFloatValue();

    return fallback;
}

bool getBoolParameterValue (Parameter* parameter, bool fallback)
{
    if (auto* booleanParameter = dynamic_cast<BooleanParameter*> (parameter))
        return booleanParameter->getBoolValue();

    return fallback;
}

String getStreamDeviceName (const DataStream* stream)
{
    if (stream == nullptr || stream->device == nullptr)
        return {};

    return stream->device->getName();
}

static constexpr const char* syncedThresholdParameterNames[] = {
    kRmsThresholdParam,
    kRmsFailChannelPercentageParam,
    kPowerlineSNRThreshParam,
    kSpectrumFailChannelPercentageParam,
    kSnapshotSaturationThresholdParam,
    kSnapshotFailChannelPercentageParam,
    kSpikeFailHzParam,
    kSpikeFailChannelPercentageParam
};
} // namespace

// ── ProbeProcessingState::allocate() ─────────────────────────────────────────

void ProbeProcessingState::allocate (int nCh, int windowSamples, int snapSamples)
{
    rmsWindowSamples = windowSamples;
    rmsSumSq.assign (nCh, 0.0);
    rmsSampleCount = 0;

    fft = std::make_unique<FFTProcessor> (FFT_SIZE);
    fftRing.assign (nCh * FFT_SIZE, 0.0f);
    fftRingPos = 0;
    fftWinCount = 0;
    powerAccum.assign (nCh * FFT_BINS, 0.0);

    spikeThreshV.assign (nCh, 100e-6f); // 100 µV bootstrap
    wasBelowThresh.assign (nCh, true);
    spikeCount.assign (nCh, 0);
    spikeSampleCount = 0;
    cumSpikeCount.assign (nCh, 0LL);
    cumSpikeSamples = 0;
    spikeWarmupDone = false;

    snapshotRing.assign (nCh * snapSamples, 0.0f);
    snapshotSaturated.assign (nCh, 0);
    snapshotSamples = snapSamples;
    snapshotPos = 0;

    totalSamplesAllowed = 0;
    totalSamplesProcessed = 0;
    processingDone = false;

    scratchRms.resize (nCh);
    scratchLiveRates.resize (nCh);
    scratchLocalRates.resize (nCh);
    scratchSpec.resize (nCh * FFT_BINS);
    scratchSurround.resize (40);
    scratchPlDb.resize (nCh);
    scratchHFDb.resize (nCh);
}

// ── QualityMonitor ─────────────────────────────────────────────────────────

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
    addMaskChannelsParameter (Parameter::STREAM_SCOPE,
                              kMaskedChannelsParam,
                              "Channels",
                              "Electrode channels to include in quality monitoring",
                              true);

    addFloatParameter (Parameter::PROCESSOR_SCOPE,
                       kPowerlineHzParam,
                       "Powerline",
                       "Fundamental line frequency used by the power spectrum plot",
                       "Hz",
                       60.0f,
                       25.0f,
                       120.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kRmsThresholdParam,
                       "RMS Threshold",
                       "RMS threshold for flagging noisy channels",
                       "uV",
                       20.0f,
                       1.0f,
                       100.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kRmsFailChannelPercentageParam,
                       "RMS Channel %",
                       "Percentage of channels allowed to exceed the RMS threshold before failing",
                       "%",
                       50.0f,
                       0.0f,
                       100.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kPowerlineSNRThreshParam,
                       "Powerline SNR",
                       "Powerline SNR threshold for flagging noisy channels",
                       "dB",
                       10.0f,
                       0.0f,
                       50.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kSpectrumFailChannelPercentageParam,
                       "Spectrum Channel %",
                       "Percentage of noisy channels allowed before the spectrum metric fails",
                       "%",
                       50.0f,
                       0.0f,
                       100.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kSnapshotSaturationThresholdParam,
                       "Snapshot Saturation",
                       "Voltage threshold for flagging saturated channels in the data snapshot",
                       "uV",
                       1000.0f,
                       1000.0f,
                       10000.0f,
                       100.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kSnapshotFailChannelPercentageParam,
                       "Snapshot Channel %",
                       "Percentage of saturated channels allowed before the snapshot metric fails",
                       "%",
                       50.0f,
                       0.0f,
                       100.0f,
                       1.0f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kSpikeFailHzParam,
                       "Spike Fail",
                       "Spike-rate threshold that marks a channel as failing",
                       "Hz",
                       0.1f,
                       0.1f,
                       10.0f,
                       0.1f);

    addFloatParameter (Parameter::STREAM_SCOPE,
                       kSpikeFailChannelPercentageParam,
                       "Spike Channel %",
                       "Percentage of low-spike-rate channels allowed before the spike metric fails",
                       "%",
                       50.0f,
                       0.0f,
                       100.0f,
                       1.0f);

    addBooleanParameter (Parameter::PROCESSOR_SCOPE,
                         kSyncMatchingDeviceThresholdsParam,
                         "Sync Matching Devices",
                         "Apply stream threshold changes to streams whose device names match",
                         false);
}

void QualityMonitor::parameterValueChanged (Parameter* parameter)
{
    if (parameter == nullptr)
        return;

    const String name = parameter->getName();

    if (name.equalsIgnoreCase (kMaskedChannelsParam))
    {
        updateSettings();

        if (editor != nullptr)
        {
            MessageManager::callAsync ([ed = editor.get()]()
                                       { ed->updateVisualizer(); });
        }
        return;
    }

    if (name.equalsIgnoreCase (kPowerlineHzParam))
    {
        const float hz = getFloatParameterValue (parameter, 60.0f);
        setPowerlineHz (hz);
        // Refresh the canvas to update the power spectrum plot's vertical line and SNR calculations.
        if (editor != nullptr && editor->isVisualizerEditor())
        {
            if (auto* canvas = dynamic_cast<VisualizerEditor*> (editor.get())->canvas.get())
            {
                MessageManager::callAsync ([canvas]()
                                           { canvas->refresh(); });
            }
        }
        return;
    }

    if (name.equalsIgnoreCase (kSyncMatchingDeviceThresholdsParam))
    {
        setSyncMatchingDeviceThresholds (getBoolParameterValue (parameter, false));
        return;
    }

    if (parameter->getScope() != Parameter::STREAM_SCOPE)
        return;

    const uint16 streamId = parameter->getStreamId();
    int probeIdx = -1;
    for (int i = 0; i < (int) probeStreamIds.size(); ++i)
    {
        if (probeStreamIds[(size_t) i] == streamId)
        {
            probeIdx = i;
            break;
        }
    }

    if (probeIdx < 0)
        return;

    if (name.equalsIgnoreCase (kRmsThresholdParam))
    {
        const float value = getFloatParameterValue (parameter, 20.0f);
        setRmsThreshold (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kRmsFailChannelPercentageParam))
    {
        const float value = getFloatParameterValue (parameter, 50.0f);
        setRmsFailChannelPercentage (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kPowerlineSNRThreshParam))
    {
        const float value = getFloatParameterValue (parameter, 10.0f);
        setPowerlineSNRThreshold (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kSpectrumFailChannelPercentageParam))
    {
        const float value = getFloatParameterValue (parameter, 50.0f);
        setSpectrumFailChannelPercentage (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kSnapshotSaturationThresholdParam))
    {
        const float value = getFloatParameterValue (parameter, SNAPSHOT_SATURATION_THRESHOLD_UV);
        setSnapshotSaturationThreshold (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kSnapshotFailChannelPercentageParam))
    {
        const float value = getFloatParameterValue (parameter, 50.0f);
        setSnapshotFailChannelPercentage (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kSpikeFailHzParam))
    {
        const float value = getFloatParameterValue (parameter, 0.1f);
        setSpikeRateThreshold (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }

    if (name.equalsIgnoreCase (kSpikeFailChannelPercentageParam))
    {
        const float value = getFloatParameterValue (parameter, 50.0f);
        setSpikeFailChannelPercentage (probeIdx, value);
        applyThresholdToMatchingDeviceStreams (streamId, name, value);
        return;
    }
}

void QualityMonitor::updateSettings()
{
    // Enumerate all data streams; keep only those with at least one ELECTRODE (DATA) channel
    Array<const DataStream*> dataStreams = getDataStreams();

    Array<const DataStream*> validStreams;
    std::vector<std::vector<int>> channelIndicesPerStream;
    std::vector<std::vector<int>> channelOrdersPerStream; // sorted pos → original electrode number

    int globalOff = 0;
    for (auto* stream : dataStreams)
    {
        // Collect electrode channel local and global indices
        std::vector<int> elecLocalIdx;
        std::vector<int> elecGlobalIdx;
        const Array<ContinuousChannel*> channels = stream->getContinuousChannels();
        for (int localIdx = 0; localIdx < channels.size(); ++localIdx)
        {
            if (channels[localIdx]->getChannelType() == ContinuousChannel::ELECTRODE)
            {
                elecLocalIdx.push_back (localIdx);
                elecGlobalIdx.push_back (globalOff + localIdx);
            }
        }

        if (! elecLocalIdx.empty())
        {
            auto* channelsParam = dynamic_cast<MaskChannelsParameter*> (stream->getParameter (kMaskedChannelsParam));
            const int availableElectrodes = (int) elecLocalIdx.size();
            std::vector<int> selectedElectrodeNumbers;

            if (channelsParam != nullptr)
            {
                channelsParam->setChannelCount (availableElectrodes);

                Array<int> selectedChannels = channelsParam->getArrayValue();
                if (selectedChannels.isEmpty())
                {
                    for (int i = 0; i < availableElectrodes; ++i)
                        selectedElectrodeNumbers.push_back (i);
                }
                else
                {
                    for (int i = 0; i < selectedChannels.size(); ++i)
                    {
                        const int selected = selectedChannels[i];
                        if (selected >= 0 && selected < availableElectrodes)
                            selectedElectrodeNumbers.push_back (selected);
                    }
                }
            }

            if (selectedElectrodeNumbers.empty())
            {
                selectedElectrodeNumbers.resize ((size_t) availableElectrodes);
                std::iota (selectedElectrodeNumbers.begin(), selectedElectrodeNumbers.end(), 0);
            }

            std::vector<int> filteredLocalIdx;
            std::vector<int> filteredGlobalIdx;
            filteredLocalIdx.reserve (selectedElectrodeNumbers.size());
            filteredGlobalIdx.reserve (selectedElectrodeNumbers.size());

            for (int selected : selectedElectrodeNumbers)
            {
                filteredLocalIdx.push_back (elecLocalIdx[(size_t) selected]);
                filteredGlobalIdx.push_back (elecGlobalIdx[(size_t) selected]);
            }

            elecLocalIdx = std::move (filteredLocalIdx);
            elecGlobalIdx = std::move (filteredGlobalIdx);

            // ── Depth-sort channels ──────────────────────────────────────────────
            // Compatible with both legacy metadata (position.y encodes shank+xpos)
            // and new metadata ("channel.ypos" = raw y, position.x, group.number).
            const int nElec = (int) elecLocalIdx.size();
            std::vector<float> depths (nElec);
            std::vector<float> xposValues (nElec);
            std::vector<int> groups (nElec, 0);
            bool allSame = true;
            bool anyYposMetadata = false;
            bool anyXposMetadata = false;
            bool anyGroupMetadata = false;
            float last = 0.0f;

            for (int i = 0; i < nElec; ++i)
            {
                const auto* ch = channels[elecLocalIdx[(size_t) i]];

                // Legacy: position.y encodes depth + shank*10000 + xpos*0.001
                float ypos = ch->position.y;

                // New metadata (added in neuropixels-pxi 2.1.1): raw y-position
                const int yposMetadataIndex = ch->findMetadata (
                    MetadataDescriptor::MetadataType::FLOAT, 1, "channel.ypos");
                if (yposMetadataIndex >= 0)
                {
                    if (const auto* yposValue = ch->getMetadataValue (yposMetadataIndex))
                    {
                        yposValue->getValue (ypos);
                        anyYposMetadata = true;
                        anyXposMetadata = true;
                    }
                }

                depths[(size_t) i] = ypos;
                xposValues[(size_t) i] = ch->position.x;
                groups[(size_t) i] = ch->group.number;

                if (! ch->group.name.equalsIgnoreCase ("default"))
                    anyGroupMetadata = true;

                if (i == 0)
                    last = depths[(size_t) i];
                else if (depths[(size_t) i] != last)
                    allSame = false;
            }

            const bool positionMetadataAvailable = anyYposMetadata && anyXposMetadata;
            const bool groupMetadataAvailable = anyGroupMetadata;

            // Only sort if there is meaningful depth/group information
            if (groupMetadataAvailable || ! allSame || anyYposMetadata)
            {
                std::vector<int> order ((size_t) nElec);
                std::iota (order.begin(), order.end(), 0);
                std::sort (order.begin(), order.end(), [&] (int a, int b)
                           {
                    const float depthDiff    = depths[(size_t) a] - depths[(size_t) b];
                    const float depthEpsilon = 1.0e-3f;
                    if (groupMetadataAvailable && groups[(size_t) a] != groups[(size_t) b])
                        return groups[(size_t) a] < groups[(size_t) b];
                    if (std::abs (depthDiff) >= depthEpsilon)
                        return depths[(size_t) a] < depths[(size_t) b];
                    if (positionMetadataAvailable)
                        return xposValues[(size_t) a] < xposValues[(size_t) b];
                    return a < b; });

                std::vector<int> sorted ((size_t) nElec);
                std::vector<int> sortedChannelOrder ((size_t) nElec);
                for (int i = 0; i < nElec; ++i)
                {
                    sorted[(size_t) i] = elecGlobalIdx[order[(size_t) i]];
                    sortedChannelOrder[(size_t) i] = selectedElectrodeNumbers[(size_t) order[(size_t) i]];
                }
                elecGlobalIdx = std::move (sorted);

                channelOrdersPerStream.push_back (std::move (sortedChannelOrder));
            }
            else
            {
                LOGD ("No depth/group metadata for stream '", stream->getName(), "'; using original channel order");
                channelOrdersPerStream.push_back (std::move (selectedElectrodeNumbers));
            }
            // ── End depth-sort ───────────────────────────────────────────────────

            validStreams.add (stream);
            channelIndicesPerStream.push_back (std::move (elecGlobalIdx));
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
            const float sr = validStreams[pi]->getSampleRate();
            const int nCh = (int) probeChannelIndices[pi].size();
            const int dur = durationSeconds.load();
            auto* rmsThresholdParam = validStreams[pi]->getParameter (kRmsThresholdParam);
            auto* rmsFailPercentageParam = validStreams[pi]->getParameter (kRmsFailChannelPercentageParam);
            auto* powerlineSNRThreshParam = validStreams[pi]->getParameter (kPowerlineSNRThreshParam);
            auto* spectrumFailPercentageParam = validStreams[pi]->getParameter (kSpectrumFailChannelPercentageParam);
            auto* snapshotSaturationThresholdParam = validStreams[pi]->getParameter (kSnapshotSaturationThresholdParam);
            auto* snapshotFailPercentageParam = validStreams[pi]->getParameter (kSnapshotFailChannelPercentageParam);
            auto* spikeFailHzParam = validStreams[pi]->getParameter (kSpikeFailHzParam);
            auto* spikeFailPercentageParam = validStreams[pi]->getParameter (kSpikeFailChannelPercentageParam);
            auto* powerlineHzParam = getParameter (kPowerlineHzParam);

            auto& currentMetrics = probeMetrics.getReference (pi);

            // Rebuild from a clean state so stale metric values and status cannot survive reconfiguration.
            ProbeMetrics resetMetrics;
            resetMetrics.rmsThresholdUV = getFloatParameterValue (rmsThresholdParam, currentMetrics.rmsThresholdUV);
            resetMetrics.rmsFailChannelPercentage = getFloatParameterValue (rmsFailPercentageParam, currentMetrics.rmsFailChannelPercentage);
            resetMetrics.spikeRateFailHz = getFloatParameterValue (spikeFailHzParam, currentMetrics.spikeRateFailHz);
            resetMetrics.spikeFailChannelPercentage = getFloatParameterValue (spikeFailPercentageParam, currentMetrics.spikeFailChannelPercentage);
            resetMetrics.powerlineHz = getFloatParameterValue (powerlineHzParam, currentMetrics.powerlineHz);
            resetMetrics.powerlineSNRThresh = getFloatParameterValue (powerlineSNRThreshParam, currentMetrics.powerlineSNRThresh);
            resetMetrics.spectrumFailChannelPercentage = getFloatParameterValue (spectrumFailPercentageParam, currentMetrics.spectrumFailChannelPercentage);
            resetMetrics.snapshotSaturationThresholdUV = getFloatParameterValue (snapshotSaturationThresholdParam, currentMetrics.snapshotSaturationThresholdUV);
            resetMetrics.snapshotFailChannelPercentage = getFloatParameterValue (snapshotFailPercentageParam, currentMetrics.snapshotFailChannelPercentage);

            resetMetrics.allocate (nCh, sr, dur);
            resetMetrics.streamName = validStreams[pi]->getName();
            resetMetrics.channelOrder = channelOrdersPerStream[pi];
            currentMetrics = std::move (resetMetrics);
        }
    }

    // ProbeProcessingState owns FFTProcessor (RAII) — safe to reconstruct
    procState.resize (totalProbes);
    for (int pi = 0; pi < totalProbes; ++pi)
    {
        const int nCh = (int) probeChannelIndices[pi].size();
        const float sr = validStreams[pi]->getSampleRate();
        const int dur = durationSeconds.load();
        procState[pi].allocate (nCh, probeMetrics.getReference (pi).rmsWindowSamples, probeMetrics.getReference (pi).snapshotSamples);
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

                // RMS accumulation + spike detection in a single pass per channel
                for (int c = 0; c < nCh; ++c)
                {
                    const float* src = buffer.getReadPointer (chIndices[c]) + rmsOffset;
                    const float thr = ps.spikeThreshV[c];
                    bool belowPrev = ps.wasBelowThresh[c];
                    double sq = 0.0;
                    for (int i = 0; i < chunk; ++i)
                    {
                        const float val = src[i];
                        sq += double (val) * double (val);
                        const bool below = val < -thr;
                        if (below && ! belowPrev)
                            ps.spikeCount[c]++;
                        belowPrev = below;
                    }
                    ps.rmsSumSq[c] += sq;
                    ps.wasBelowThresh[c] = belowPrev;
                }

                ps.rmsSampleCount += chunk;
                ps.spikeSampleCount += chunk;
                rmsOffset += chunk;
                rmsRemain -= chunk;

                if (ps.rmsSampleCount >= ps.rmsWindowSamples)
                {
                    finalizeRms (pi);
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
        if (ps.totalSamplesAllowed > 0 && ps.totalSamplesProcessed >= ps.totalSamplesAllowed)
        {
            // Force-finalize any partial windows
            if (ps.rmsSampleCount > 0)
            {
                finalizeRms (pi);
                finalizeSpikes (pi);
            }
            if (ps.fftWinCount > 0)
                finalizeFFT (pi);

            ps.processingDone = true;
            {
                std::lock_guard<std::mutex> lock (metricsMutex);
                auto& m = probeMetrics.getReference (pi);
                m.numSaturatedChannels = 0;
                for (uint8_t saturated : ps.snapshotSaturated)
                    if (saturated != 0)
                        ++m.numSaturatedChannels;

                m.processingDone = true;
                m.finalizeStatuses();
            }

            // If every probe is now done, clear the flag so a new Capture run
            // can be started without stopping/restarting acquisition.
            bool allDone = true;
            for (int i = 0; i < totalProbes; ++i)
                if (! procState[i].processingDone)
                {
                    allDone = false;
                    break;
                }
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

    for (int c = 0; c < nCh; ++c)
    {
        const float rms = float (std::sqrt (ps.rmsSumSq[c] / n));
        ps.scratchRms[c] = rms;
        ps.spikeThreshV[c] = std::max (rms * 5.0f, 1e-7f); // adaptive spike thr
        ps.rmsSumSq[c] = 0.0;
    }
    ps.rmsSampleCount = 0;

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.rmsUV = ps.scratchRms;
    m.numHighRmsChannels = 0;
    for (float v : m.rmsUV)
        if (v > m.rmsThresholdUV)
            m.numHighRmsChannels++;

    // Append this frame to the rolling RMS history heatmap
    if (m.rmsHistoryFrames < m.rmsHistoryMaxFrames)
    {
        const int frameOff = m.rmsHistoryFrames * nCh;
        for (int c = 0; c < nCh; ++c)
            m.rmsHistory[frameOff + c] = ps.scratchRms[c];
        m.rmsHistoryFrames++;
    }

    // Linearize snapshot ring buffer into probeMetrics (lock already held)
    {
        const int writePos = ps.snapshotPos;
        const int tail = ps.snapshotSamples - writePos;
        for (int c = 0; c < nCh; ++c)
        {
            const float* ring = ps.snapshotRing.data() + c * ps.snapshotSamples;
            float* dst = m.dataSnapshot.data() + c * ps.snapshotSamples;
            std::copy (ring + writePos, ring + ps.snapshotSamples, dst);
            std::copy (ring, ring + writePos, dst + tail);
        }
    }
}

void QualityMonitor::finalizeFFT (int pi)
{
    auto& ps = procState[pi];
    const int nCh = (int) probeChannelIndices[pi].size();
    const int nWin = std::max (1, ps.fftWinCount);

    // Average accumulated power and clear accumulators (into pre-allocated scratch)
    for (int c = 0; c < nCh; ++c)
    {
        double* acc = ps.powerAccum.data() + c * FFT_BINS;
        float* dest = ps.scratchSpec.data() + c * FFT_BINS;
        for (int k = 0; k < FFT_BINS; ++k)
        {
            dest[k] = float (acc[k] / nWin);
            acc[k] = 0.0;
        }
    }
    ps.fftWinCount = 0;

    // Snapshot read-only fields before entering the lock
    const float sr = probeMetrics[pi].sampleRate;
    const float plHz = probeMetrics[pi].powerlineHz;
    const float snrThr = probeMetrics[pi].powerlineSNRThresh;

    const float nyquist = sr / 2.0f;
    const float hzPerBin = sr / float (FFT_SIZE);
    auto hzToBin = [&] (float hz) -> int
    {
        return jlimit (0, FFT_BINS - 1, int (hz / hzPerBin));
    };

    // Band limits (computed once, reused per channel)
    const int plBin = hzToBin (plHz);
    const int kRefStart = hzToBin (500.0f);
    const int kRefEnd = std::min (FFT_BINS - 1, hzToBin (5000.0f));
    const int kHFStart = hzToBin (8000.0f);
    const int kHFEnd = std::min (FFT_BINS - 1, hzToBin (jmin (15000.0f, nyquist)));

    int numNoisyCh = 0;
    for (int c = 0; c < nCh; ++c)
    {
        const float* row = ps.scratchSpec.data() + c * FFT_BINS;

        // --- Per-channel powerline band power: mean of ±3 bins around fundamental ---
        {
            float plSum = 0.0f;
            int plCnt = 0;
            for (int k = plBin - 3; k <= plBin + 3; ++k)
            {
                if (k >= 1 && k < FFT_BINS && row[k] > 0.0f)
                {
                    plSum += row[k];
                    ++plCnt;
                }
            }
            ps.scratchPlDb[c] = (plCnt > 0 && plSum > 0.0f)
                                    ? 10.0f * std::log10 (plSum / float (plCnt))
                                    : 0.0f;
        }

        // --- Per-channel HF band power: mean over 8–15 kHz ---
        float hfSum = 0.0f;
        int hfCnt = 0;
        for (int k = kHFStart; k <= kHFEnd; ++k)
            if (row[k] > 0.0f)
            {
                hfSum += row[k];
                ++hfCnt;
            }
        ps.scratchHFDb[c] = (hfCnt > 0 && hfSum > 0.0f)
                                ? 10.0f * std::log10 (hfSum / float (hfCnt))
                                : 0.0f;

        // --- Noisy channel detection ---
        bool noisy = false;

        // 1. Powerline SNR: peak bin vs. median of ±20 bins (excluding ±3 around peak)
        if (plHz > 0.0f && plHz < nyquist && plBin > 0 && plBin < FFT_BINS - 1)
        {
            int surroundCount = 0;
            for (int kb = plBin - 20; kb <= plBin + 20; ++kb)
            {
                if (kb < 0 || kb >= FFT_BINS)
                    continue;
                if (std::abs (kb - plBin) <= 3)
                    continue;
                ps.scratchSurround[surroundCount++] = row[kb];
            }
            if (surroundCount > 0)
            {
                std::sort (ps.scratchSurround.begin(), ps.scratchSurround.begin() + surroundCount);
                const float med = ps.scratchSurround[surroundCount / 2];
                if (med >= 1e-30f)
                {
                    const float snrDb = 10.0f * std::log10 (row[plBin] / med);
                    if (snrDb > snrThr)
                        noisy = true;
                }
            }
        }

        // 2. HF noise: mean power in 8–15 kHz elevated vs. reference band (500 Hz – 5 kHz)
        if (! noisy && kHFEnd > kHFStart && kRefEnd > kRefStart && hfCnt > 0)
        {
            float refSum = 0.0f;
            int refCnt = 0;
            for (int k = kRefStart; k <= kRefEnd; ++k)
                if (row[k] > 0.0f)
                {
                    refSum += row[k];
                    ++refCnt;
                }
            if (refCnt > 0 && refSum > 1e-30f)
            {
                const float snrDb = 10.0f * std::log10 ((hfSum / float (hfCnt)) / (refSum / float (refCnt)));
                if (snrDb > snrThr)
                    noisy = true;
            }
        }

        if (noisy)
            ++numNoisyCh;
    }

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.powerSpectrum = ps.scratchSpec;
    m.channelPowerlineDb = ps.scratchPlDb;
    m.channelHFNoiseDb = ps.scratchHFDb;
    m.numNoisyChannels = numNoisyCh;
}

void QualityMonitor::finalizeSpikes (int pi)
{
    auto& ps = procState[pi];
    const int nCh = (int) probeChannelIndices[pi].size();

    // First window used bootstrap threshold — discard its counts so spikes
    // detected before the adaptive threshold was calibrated don't bias the average.
    if (! ps.spikeWarmupDone)
    {
        std::fill (ps.spikeCount.begin(), ps.spikeCount.end(), 0);
        ps.spikeSampleCount = 0;
        ps.spikeWarmupDone = true;
        return;
    }

    // Compute per-window live rate BEFORE accumulating into cumulative totals
    const float windowSec = std::max (float (ps.spikeSampleCount) / probeMetrics[pi].sampleRate, 0.001f);
    for (int c = 0; c < nCh; ++c)
        ps.scratchLiveRates[c] = float (ps.spikeCount[c]) / windowSec;

    // Accumulate window counts into run totals, then reset the window
    for (int c = 0; c < nCh; ++c)
    {
        ps.cumSpikeCount[c] += ps.spikeCount[c];
        ps.spikeCount[c] = 0;
    }
    ps.cumSpikeSamples += ps.spikeSampleCount;
    ps.spikeSampleCount = 0;

    // Average rate over the entire run so far
    const float totalElapsedSec = std::max (
        float (ps.cumSpikeSamples) / probeMetrics[pi].sampleRate, 0.001f);

    for (int c = 0; c < nCh; ++c)
        ps.scratchLocalRates[c] = float (ps.cumSpikeCount[c]) / totalElapsedSec;

    std::lock_guard<std::mutex> lock (metricsMutex);
    auto& m = probeMetrics.getReference (pi);
    m.spikeRateHz = ps.scratchLocalRates;
    m.spikeRateLiveHz = ps.scratchLiveRates;

    // Accumulate live rates into the spike-rate history heatmap
    if (m.spikeRateHistoryFrames < m.rmsHistoryMaxFrames)
    {
        const int frame = m.spikeRateHistoryFrames;
        for (int c = 0; c < nCh; ++c)
            m.spikeRateHistory[frame * nCh + c] = ps.scratchLiveRates[c];
        ++m.spikeRateHistoryFrames;
    }

    m.numLowSpikeChannels = 0;
    for (float r : m.spikeRateHz)
        if (r < m.spikeRateFailHz)
            m.numLowSpikeChannels++;
}

void QualityMonitor::captureSnapshot (int pi, AudioBuffer<float>& buffer)
{
    const auto& chIndices = probeChannelIndices[pi];
    const int nCh = (int) chIndices.size();
    const int numSamples = getNumSamplesInBlock (probeStreamIds[pi]);
    auto& ps = procState[pi];
    const int N = std::min (numSamples, ps.snapshotSamples);
    const int pos = ps.snapshotPos;
    const int wrap = ps.snapshotSamples - pos;
    const float saturationThresholdUV = probeMetrics[pi].snapshotSaturationThresholdUV;

    // Channel-major: at most two contiguous copies per channel to handle the ring wrap
    for (int c = 0; c < nCh; ++c)
    {
        const float* src = buffer.getReadPointer (chIndices[c]);
        float* ring = ps.snapshotRing.data() + c * ps.snapshotSamples;
        uint8_t& saturated = ps.snapshotSaturated[(size_t) c];
        for (int s = 0; s < N; ++s)
            if (std::abs (src[s]) >= saturationThresholdUV)
                saturated = 1;

        if (N <= wrap)
        {
            std::copy (src, src + N, ring + pos);
        }
        else
        {
            std::copy (src, src + wrap, ring + pos);
            std::copy (src + wrap, src + N, ring);
        }
    }

    ps.snapshotPos = (pos + N) % ps.snapshotSamples;
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

void QualityMonitor::setRmsFailChannelPercentage (int pi, float percentage)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        auto& metrics = probeMetrics.getReference (pi);
        metrics.rmsFailChannelPercentage = percentage;
        metrics.finalizeStatuses();
    }
}

void QualityMonitor::setSpikeRateThreshold (int pi, float failHz)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        probeMetrics.getReference (pi).spikeRateFailHz = failHz;
    }
}

void QualityMonitor::setSpikeFailChannelPercentage (int pi, float percentage)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        auto& metrics = probeMetrics.getReference (pi);
        metrics.spikeFailChannelPercentage = percentage;
        metrics.finalizeStatuses();
    }
}

void QualityMonitor::setPowerlineSNRThreshold (int pi, float snrThreshDb)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
        probeMetrics.getReference (pi).powerlineSNRThresh = snrThreshDb;
}

void QualityMonitor::setSpectrumFailChannelPercentage (int pi, float percentage)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        auto& metrics = probeMetrics.getReference (pi);
        metrics.spectrumFailChannelPercentage = percentage;
        metrics.finalizeStatuses();
    }
}

void QualityMonitor::setSnapshotSaturationThreshold (int pi, float uv)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
        probeMetrics.getReference (pi).snapshotSaturationThresholdUV = uv;
}

void QualityMonitor::setSnapshotFailChannelPercentage (int pi, float percentage)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    if (pi < probeMetrics.size())
    {
        auto& metrics = probeMetrics.getReference (pi);
        metrics.snapshotFailChannelPercentage = percentage;
        metrics.finalizeStatuses();
    }
}

void QualityMonitor::setPowerlineHz (float hz)
{
    std::lock_guard<std::mutex> lock (metricsMutex);
    for (auto& metrics : probeMetrics)
        metrics.powerlineHz = hz;
}

void QualityMonitor::applyThresholdToMatchingDeviceStreams (uint16 sourceStreamId, const String& parameterName, float value)
{
    if (! syncMatchingDeviceThresholds.load() || applyingMatchedDeviceThresholds)
        return;

    const auto dataStreams = getDataStreams();
    const DataStream* sourceStream = nullptr;
    for (auto* stream : dataStreams)
    {
        if (stream != nullptr && stream->getStreamId() == sourceStreamId)
        {
            sourceStream = stream;
            break;
        }
    }

    const String sourceDeviceName = getStreamDeviceName (sourceStream);
    if (sourceDeviceName.isEmpty())
        return;

    ScopedValueSetter<bool> guard (applyingMatchedDeviceThresholds, true);
    for (auto* stream : dataStreams)
    {
        if (stream == nullptr || stream->getStreamId() == sourceStreamId)
            continue;

        if (getStreamDeviceName (stream) != sourceDeviceName)
            continue;

        auto* targetParameter = stream->getParameter (parameterName);
        auto* targetFloatParameter = dynamic_cast<FloatParameter*> (targetParameter);
        if (targetFloatParameter == nullptr)
            continue;

        if (std::abs (targetFloatParameter->getFloatValue() - value) < 1.0e-6f)
            continue;

        targetParameter->setNextValue (value);
    }
}

void QualityMonitor::syncThresholdsToMatchingDeviceStreams (int probeIdx)
{
    if (! syncMatchingDeviceThresholds.load())
        return;

    const uint16 sourceStreamId = getProbeStreamId (probeIdx);
    if (sourceStreamId == 0)
        return;

    auto* sourceStream = getDataStream (sourceStreamId);
    if (sourceStream == nullptr || getStreamDeviceName (sourceStream).isEmpty())
        return;

    for (auto* parameterName : syncedThresholdParameterNames)
    {
        auto* sourceParameter = dynamic_cast<FloatParameter*> (sourceStream->getParameter (parameterName));
        if (sourceParameter == nullptr)
            continue;

        applyThresholdToMatchingDeviceStreams (sourceStreamId, parameterName, sourceParameter->getFloatValue());
    }
}

uint16 QualityMonitor::getProbeStreamId (int probeIdx) const
{
    if (probeIdx < 0 || probeIdx >= (int) probeStreamIds.size())
        return 0;

    return probeStreamIds[(size_t) probeIdx];
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

void QualityMonitor::stopProcessing()
{
    processingHasStarted.store (false);
}

void QualityMonitor::setAutoStart (bool enabled)
{
    autoStartProcessing.store (enabled);
}

void QualityMonitor::setSyncMatchingDeviceThresholds (bool enabled)
{
    syncMatchingDeviceThresholds.store (enabled);
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
        ps.processingDone = false;
        ps.rmsSampleCount = 0;
        std::fill (ps.rmsSumSq.begin(), ps.rmsSumSq.end(), 0.0);
        std::fill (ps.spikeCount.begin(), ps.spikeCount.end(), 0);
        ps.spikeSampleCount = 0;
        std::fill (ps.cumSpikeCount.begin(), ps.cumSpikeCount.end(), 0LL);
        ps.cumSpikeSamples = 0;
        ps.spikeWarmupDone = false;
        ps.fftRingPos = 0;
        ps.fftWinCount = 0;
        std::fill (ps.powerAccum.begin(), ps.powerAccum.end(), 0.0);
        std::fill (ps.snapshotRing.begin(), ps.snapshotRing.end(), 0.0f);
        std::fill (ps.snapshotSaturated.begin(), ps.snapshotSaturated.end(), uint8_t (0));
        ps.snapshotPos = 0;
    }

    {
        std::lock_guard<std::mutex> lock (metricsMutex);
        for (int pi = 0; pi < totalProbes; ++pi)
        {
            auto& m = probeMetrics.getReference (pi);
            const float sr = m.sampleRate;
            const int nCh = m.numChannels;
            const int maxFrames = std::max (1, int (float (dur) * sr / float (m.rmsWindowSamples)));

            m.processingDone = false;
            m.resetStatuses();
            m.numHighRmsChannels = 0;
            m.numLowSpikeChannels = 0;
            m.numNoisyChannels = 0;
            m.numSaturatedChannels = 0;
            m.rmsHistoryFrames = 0;
            m.spikeRateHistoryFrames = 0;
            m.analysisDurationSec = dur;
            m.rmsHistoryMaxFrames = maxFrames;
            m.rmsUV.assign (nCh, 0.0f);
            m.spikeRateHz.assign (nCh, 0.0f);
            m.spikeRateLiveHz.assign (nCh, 0.0f);
            m.powerSpectrum.assign (nCh * FFT_BINS, 0.0f);
            m.dataSnapshot.assign (nCh * m.snapshotSamples, 0.0f);
            m.rmsHistory.assign (nCh * maxFrames, 0.0f);
            m.spikeRateHistory.assign (nCh * maxFrames, 0.0f);
            m.channelPowerlineDb.assign (nCh, 0.0f);
            m.channelHFNoiseDb.assign (nCh, 0.0f);

            procState[pi].totalSamplesAllowed =
                int64_t (dur) * int64_t (sr + 0.5f);
        }
    }

    processingHasStarted.store (true);
}