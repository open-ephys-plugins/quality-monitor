/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
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

#include "QualityMonitorCanvas.h"

#include "QualityMonitor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

// -- QCColours -----------------------------------------------

namespace QCColours
{
Colour inferno (float t)
{
    t = jlimit (0.0f, 1.0f, t);
    float r = jlimit (0.0f, 1.0f, 1.4642f * t - 0.0576f * t * t - 0.001f);
    float g = jlimit (0.0f, 1.0f, -0.0282f + 1.8463f * t - 1.8463f * t * t + 0.72f * t * t * t);
    float b = jlimit (0.0f, 1.0f, t < 0.35f ? 0.52f * t / 0.35f : 0.52f - 1.48f * (t - 0.35f));
    return Colour::fromFloatRGBA (r, g, b, 1.0f);
}

Colour cividis (float t)
{
    t = jlimit (0.0f, 1.0f, t);
    float r = jlimit (0.0f, 1.0f, -0.0150f + 1.0638f * t);
    float g = jlimit (0.0f, 1.0f, 0.1260f + 0.6863f * t);
    float b = jlimit (0.0f, 1.0f, 0.5320f - 0.2880f * t + 0.1600f * t * t);
    return Colour::fromFloatRGBA (r, g, b, 1.0f);
}

Colour statusCol (ProbeStatus s)
{
    switch (s)
    {
        case ProbeStatus::PASS:
            return Colour (0xff4caf50);
        case ProbeStatus::WARN:
            return Colour (0xffff9800);
        case ProbeStatus::FAIL:
            return Colour (0xfff44336);
        default:
            return Colours::grey;
    }
}

String statusStr (ProbeStatus s)
{
    switch (s)
    {
        case ProbeStatus::PASS:
            return "PASS";
        case ProbeStatus::WARN:
            return "WARN";
        case ProbeStatus::FAIL:
            return "FAIL";
        default:
            return "...";
    }
}
} // namespace QCColours

static constexpr int TITLE_H = 20;
static constexpr int META_H = 18;
static constexpr int PLOT_PAD = 4;
static constexpr int AXIS_L = 28;
static constexpr int AXIS_B = 18;
static constexpr int CBAR_W = 14;

static void drawBadge (Graphics& g, Rectangle<int> r, Colour bg, const String& text)
{
    g.setColour (bg);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);
    g.setColour (Colours::white);
    g.setFont (Font (10.0f, Font::bold));
    g.drawText (text, r, Justification::centred, false);
}

// ─── RmsHeatmapPanel ─────────────────────────────────────────────────────────

void RmsHeatmapPanel::updateData (const ProbeMetrics& m)
{
    rmsUV = m.rmsUV;
    numCh = m.numChannels;
    threshUV = m.rmsThresholdUV;
    numHighRms = m.numHighRmsChannels;
    maxRms = rmsUV.empty() ? 1.0f : *std::max_element (rmsUV.begin(), rmsUV.end());
    maxRms = std::max (maxRms, 1.0f);
    repaint();
}

void RmsHeatmapPanel::drawColourBar (Graphics& g, Rectangle<float> r)
{
    for (int y = 0; y < int (r.getHeight()); ++y)
    {
        float t = 1.0f - float (y) / r.getHeight();
        g.setColour (QCColours::inferno (t));
        g.fillRect (r.getX(), r.getY() + y, r.getWidth(), 1.0f);
    }
    g.setColour (Colours::grey);
    g.setFont (Font (8.5f));
    g.drawText ("High", int (r.getX()), int (r.getY()), int (r.getWidth()) + 6, 10, Justification::centredLeft);
    g.drawText ("Low", int (r.getX()), int (r.getBottom()) - 10, int (r.getWidth()) + 6, 10, Justification::centredLeft);
}

void RmsHeatmapPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (Colour (0xff1c1c1c));

    g.setColour (Colours::white);
    g.setFont (Font (13.0f, Font::bold));
    g.drawText ("RMS Heatmap", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (Colours::grey);
    g.setFont (Font (10.5f));
    g.drawText ("THRESHOLD  " + String (threshUV, 0) + " \xc2\xb5V", metaRow.removeFromLeft (160), Justification::centredLeft);
    if (numHighRms > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffc62828), String (numHighRms) + " ch above " + String (threshUV, 0) + " \xc2\xb5V threshold");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (rmsUV.empty())
        return;

    auto cbarBounds = b.removeFromRight (CBAR_W + 2).toFloat().reduced (0, 4);
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);

    float cellH = float (b.getHeight()) / float (numCh);
    for (int c = 0; c < numCh; ++c)
    {
        g.setColour (QCColours::inferno (rmsUV[c] / maxRms));
        g.fillRect (float (b.getX()), float (b.getY()) + c * cellH, float (b.getWidth()), std::max (cellH, 1.0f));
    }

    float ty = float (b.getY()) + float (b.getHeight()) * jlimit (0.0f, 1.0f, threshUV / maxRms);
    g.setColour (Colours::white.withAlpha (0.55f));
    g.drawHorizontalLine (int (ty), float (b.getX()), float (b.getRight()));
    g.setFont (Font (8.0f));
    g.drawText (String (threshUV, 0), b.getX() - AXIS_L, int (ty) - 5, AXIS_L - 2, 10, Justification::centredRight);

    g.setColour (Colours::darkgrey);
    g.setFont (Font (9.0f));
    for (int tick : { 0, 96, 192, 288, 383 })
    {
        if (tick >= numCh)
            break;
        float y = float (b.getY()) + (float (tick) / float (numCh)) * float (b.getHeight());
        g.drawText (String (tick), b.getX() - AXIS_L, int (y) - 5, AXIS_L - 2, 10, Justification::centredRight);
        g.drawHorizontalLine (int (y), float (b.getX()), float (b.getX()) + 4.0f);
    }
    g.drawText ("RMS Voltage (\xc2\xb5V)", b.getX(), b.getBottom(), b.getWidth(), AXIS_B, Justification::centred);
    drawColourBar (g, cbarBounds);
}

// ─── PowerSpectrumPanel ───────────────────────────────────────────────────────

void PowerSpectrumPanel::updateData (const ProbeMetrics& m)
{
    spectrum = m.powerSpectrum;
    numCh = m.numChannels;
    sampleRate = m.sampleRate;
    powerlineHz = m.powerlineHz;
    numNoisyCh = m.numNoisyChannels;
    repaint();
}

void PowerSpectrumPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (Colour (0xff1c1c1c));

    g.setColour (Colours::white);
    g.setFont (Font (13.0f, Font::bold));
    g.drawText ("Power Spectrum", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (Colours::grey);
    g.setFont (Font (10.5f));
    g.drawText ("POWERLINE NOISE  " + String (powerlineHz, 0) + " Hz", metaRow.removeFromLeft (200), Justification::centredLeft);
    if (numNoisyCh > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffe65100), String (numNoisyCh) + " ch noisy");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (spectrum.empty())
        return;

    auto pb = b;
    pb.removeFromLeft (AXIS_L);
    pb.removeFromBottom (AXIS_B);
    float px = float (pb.getX()), py = float (pb.getY());
    float pw = float (pb.getWidth()), ph = float (pb.getHeight());
    float nyquist = sampleRate / 2.0f;

    g.setColour (Colours::white.withAlpha (0.05f));
    for (int i = 1; i <= 4; ++i)
        g.drawHorizontalLine (int (py + ph * float (i) / 4.0f), px, px + pw);

    g.setColour (Colours::darkgrey);
    g.setFont (Font (9.0f));
    for (float ft : { 0.0f, 50.0f, 100.0f, 150.0f, 200.0f, 300.0f, 500.0f })
    {
        if (ft > nyquist)
            break;
        float x = px + (ft / nyquist) * pw;
        g.drawVerticalLine (int (x), py + ph, py + ph + 4.0f);
        g.drawText (String (int (ft)), int (x) - 15, int (py + ph + 4), 30, 12, Justification::centred);
    }

    std::vector<int> tch (NUM_TRACES);
    for (int i = 0; i < NUM_TRACES; ++i)
        tch[i] = jlimit (0, numCh - 1, i * numCh / NUM_TRACES);

    float gMin = 1e20f, gMax = -1e20f;
    for (int c : tch)
    {
        const float* row = spectrum.data() + c * FFT_BINS;
        for (int k = 1; k < FFT_BINS; ++k)
        {
            float db = row[k] > 0.0f ? 10.0f * std::log10 (row[k]) : -120.0f;
            gMin = std::min (gMin, db);
            gMax = std::max (gMax, db);
        }
    }
    if (gMax <= gMin)
        gMax = gMin + 1.0f;

    Colour tc[NUM_TRACES] = { Colour (0xff00bcd4), Colour (0xffffeb3b), Colour (0xff8bc34a), Colour (0xffff9800), Colour (0xffe91e63), Colour (0xff9c27b0) };
    for (int ti = 0; ti < NUM_TRACES; ++ti)
    {
        const float* row = spectrum.data() + tch[ti] * FFT_BINS;
        Path trace;
        bool first = true;
        for (int k = 1; k < FFT_BINS; ++k)
        {
            float hz = (float (k) / float (FFT_BINS - 1)) * nyquist;
            float db = row[k] > 0.0f ? 10.0f * std::log10 (row[k]) : -120.0f;
            float x = px + (hz / nyquist) * pw;
            float y = jlimit (py, py + ph, py + ph * (1.0f - (db - gMin) / (gMax - gMin)));
            first ? trace.startNewSubPath (x, y) : trace.lineTo (x, y);
            first = false;
        }
        g.setColour (tc[ti].withAlpha (0.75f));
        g.strokePath (trace, PathStrokeType (1.3f));
    }

    float harmonics[] = { powerlineHz, powerlineHz * 2.0f, 50.0f, 100.0f };
    for (float h : harmonics)
    {
        if (h <= 0.0f || h > nyquist)
            continue;
        float x = px + (h / nyquist) * pw;
        g.setColour (Colour (0xffff9800).withAlpha (0.55f));
        g.drawVerticalLine (int (x), py, py + ph);
        g.setColour (Colour (0xffff9800));
        g.setFont (Font (8.5f));
        g.drawText (String (int (h)) + " Hz", int (x) + 2, int (py) + 2, 38, 11, Justification::centredLeft);
    }

    g.setColour (Colours::grey);
    g.setFont (Font (9.5f));
    g.addTransform (AffineTransform::rotation (-MathConstants<float>::halfPi, float (b.getX() + 8), py + ph / 2.0f));
    g.drawText ("Power (dB)", int (b.getX() + 8) - 30, int (py + ph / 2.0f) - 5, 60, 10, Justification::centred);
    g.addTransform (AffineTransform::rotation (MathConstants<float>::halfPi, float (b.getX() + 8), py + ph / 2.0f));
    g.drawText ("Frequency (Hz)", int (px), int (py + ph + 4), int (pw), 12, Justification::centred);
}

// ─── DataSnapshotPanel ────────────────────────────────────────────────────────

void DataSnapshotPanel::updateData (const ProbeMetrics& m)
{
    snapshot = m.dataSnapshot;
    numCh = m.numChannels;
    if (! snapshot.empty())
    {
        std::vector<float> abs (snapshot.size());
        std::transform (snapshot.begin(), snapshot.end(), abs.begin(), [] (float v)
                        { return std::abs (v); });
        std::sort (abs.begin(), abs.end());
        maxUV = std::max (abs[size_t (0.95f * float (abs.size()))], 1.0f);
        minUV = -maxUV;
    }
    repaint();
}

void DataSnapshotPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (Colour (0xff1c1c1c));

    g.setColour (Colours::white);
    g.setFont (Font (13.0f, Font::bold));
    g.drawText ("Data Snapshot", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);
    g.setColour (Colours::grey);
    g.setFont (Font (10.5f));
    g.drawText ("Color Map  Cividis", b.removeFromTop (META_H), Justification::centredLeft);

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (snapshot.empty())
        return;

    auto cbar = b.removeFromRight (CBAR_W + 2).toFloat().reduced (0, 4);
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);

    float range = maxUV - minUV;
    float cH = float (b.getHeight()) / float (numCh);
    float cW = float (b.getWidth()) / float (SNAPSHOT_SAMPLES);

    for (int c = 0; c < numCh; ++c)
    {
        const float* row = snapshot.data() + c * SNAPSHOT_SAMPLES;
        for (int s = 0; s < SNAPSHOT_SAMPLES; ++s)
        {
            g.setColour (QCColours::cividis (jlimit (0.0f, 1.0f, (row[s] - minUV) / range)));
            g.fillRect (float (b.getX()) + s * cW, float (b.getY()) + c * cH, std::max (cW, 1.0f), std::max (cH, 1.0f));
        }
    }

    for (int y = 0; y < int (cbar.getHeight()); ++y)
    {
        g.setColour (QCColours::cividis (1.0f - float (y) / cbar.getHeight()));
        g.fillRect (cbar.getX(), cbar.getY() + y, cbar.getWidth(), 1.0f);
    }
    g.setColour (Colours::grey);
    g.setFont (Font (8.5f));
    g.drawText ("+" + String (int (maxUV)) + "\xc2\xb5V", int (cbar.getX()), int (cbar.getY()), int (cbar.getWidth()) + 6, 10, Justification::centredLeft);
    g.drawText ("-" + String (int (maxUV)) + "\xc2\xb5V", int (cbar.getX()), int (cbar.getBottom()) - 10, int (cbar.getWidth()) + 6, 10, Justification::centredLeft);

    g.setColour (Colours::darkgrey);
    g.setFont (Font (9.0f));
    for (int tick : { 0, 96, 192, 288, 383 })
    {
        if (tick >= numCh)
            break;
        float y = float (b.getY()) + (float (tick) / float (numCh)) * float (b.getHeight());
        g.drawText (String (tick), b.getX() - AXIS_L, int (y) - 5, AXIS_L - 2, 10, Justification::centredRight);
    }
    g.setColour (Colours::grey);
    g.drawText ("Time (samples)", b.getX(), b.getBottom(), b.getWidth(), AXIS_B, Justification::centred);
}

// ─── SpikeRatePanel ───────────────────────────────────────────────────────────

void SpikeRatePanel::updateData (const ProbeMetrics& m)
{
    rateHz = m.spikeRateHz;
    numCh = m.numChannels;
    spikeFailHz = m.spikeRateFailHz;
    spikeLowHz = m.spikeRateLowHz;
    numLowCh = m.numLowSpikeChannels;
    maxRateHz = rateHz.empty() ? 30.0f : *std::max_element (rateHz.begin(), rateHz.end());
    maxRateHz = std::max (maxRateHz, spikeLowHz * 2.0f);
    repaint();
}

void SpikeRatePanel::drawLegend (Graphics& g, Rectangle<int> r)
{
    struct
    {
        Colour col;
        const char* lbl;
    } e[] = {
        { Colour (0xff42a5f5), "Normal (>=2 Hz)" },
        { Colour (0xffff9800), "Low (0.1-2 Hz)" },
        { Colour (0xfff44336), "Fail (<0.1 Hz)" }
    };
    int y = r.getY() + 2;
    for (auto& v : e)
    {
        g.setColour (v.col);
        g.fillRect (r.getX(), y, 10, 10);
        g.setColour (Colours::lightgrey);
        g.setFont (Font (9.5f));
        g.drawText (v.lbl, r.getX() + 13, y, r.getWidth() - 13, 10, Justification::centredLeft);
        y += 13;
    }
}

void SpikeRatePanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (Colour (0xff1c1c1c));

    g.setColour (Colours::white);
    g.setFont (Font (13.0f, Font::bold));
    g.drawText ("Spike Rate", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (Colours::grey);
    g.setFont (Font (10.5f));
    g.drawText ("SPIKE THRESH  " + String (spikeFailHz, 1) + " Hz", metaRow.removeFromLeft (180), Justification::centredLeft);
    if (numLowCh > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffc62828), String (numLowCh) + " ch below " + String (spikeFailHz, 1) + " Hz threshold");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (rateHz.empty())
        return;

    drawLegend (g, b.removeFromRight (110).removeFromTop (44));
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);

    float px = float (b.getX()), py = float (b.getY());
    float pw = float (b.getWidth()), ph = float (b.getHeight());

    int step = maxRateHz > 50 ? 20 : (maxRateHz > 20 ? 10 : 5);
    for (int t = 0; t <= int (maxRateHz); t += step)
    {
        float y = py + ph * (1.0f - float (t) / maxRateHz);
        g.setColour (Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine (int (y), px, px + pw);
        g.setColour (Colours::darkgrey);
        g.setFont (Font (9.0f));
        g.drawText (String (t), int (px) - AXIS_L, int (y) - 5, AXIS_L - 2, 10, Justification::centredRight);
    }

    float ty = py + ph * (1.0f - spikeFailHz / maxRateHz);
    g.setColour (Colour (0xffc62828).withAlpha (0.75f));
    g.drawHorizontalLine (int (ty), px, px + pw);
    g.setColour (Colour (0xffff5252));
    g.setFont (Font (8.5f));
    g.drawText (String (spikeFailHz, 1) + " Hz threshold", int (px + pw) - 90, int (ty) - 12, 88, 10, Justification::centredRight);

    float bW = std::max (1.0f, pw / float (numCh));
    for (int c = 0; c < numCh; ++c)
    {
        float rate = rateHz[c];
        float bH = (rate / maxRateHz) * ph;
        float x = px + float (c) / float (numCh) * pw;
        g.setColour (rate < spikeFailHz ? Colour (0xfff44336) : rate < spikeLowHz ? Colour (0xffff9800)
                                                                                  : Colour (0xff42a5f5));
        g.fillRect (x, py + ph - bH, bW, bH);
    }

    g.setColour (Colours::darkgrey);
    g.setFont (Font (9.0f));
    for (int t : { 0, 96, 192, 288, 384 })
    {
        if (t > numCh)
            break;
        float x = px + (float (t) / float (numCh)) * pw;
        g.drawVerticalLine (int (x), py + ph, py + ph + 4.0f);
        g.drawText ("Ch " + String (t), int (x) - 16, int (py + ph + 4), 34, 12, Justification::centred);
    }
}

// ─── ProbeListRow ─────────────────────────────────────────────────────────────

void ProbeListRow::setMetrics (const ProbeMetrics& m, bool selected)
{
    name = m.streamName.isEmpty() ? "Probe" : m.streamName;
    numCh = m.numChannels;
    status = m.status;
    sel = selected;
    repaint();
}

void ProbeListRow::paint (Graphics& g)
{
    auto b = getLocalBounds();
    Colour sc = QCColours::statusCol (status);
    if (sel)
    {
        g.setColour (sc.withAlpha (0.15f));
        g.fillRoundedRectangle (b.toFloat().reduced (3, 2), 6.0f);
        g.setColour (sc.withAlpha (0.70f));
        g.drawRoundedRectangle (b.toFloat().reduced (3, 2), 6.0f, 1.5f);
    }
    g.setColour (sc);
    g.fillEllipse (9.0f, float (b.getCentreY()) - 5.0f, 10.0f, 10.0f);
    g.setColour (Colours::white);
    g.setFont (Font (12.5f, Font::bold));
    g.drawText (name, 26, b.getY() + 6, 90, 16, Justification::centredLeft);
    g.setColour (Colours::grey);
    g.setFont (Font (10.0f));
    g.drawText (String (numCh) + " ch", 26, b.getY() + 24, 90, 14, Justification::centredLeft);
    drawBadge (g, b.removeFromRight (54).reduced (6, 12), sc, QCColours::statusStr (status));
}

void ProbeListRow::mouseDown (const MouseEvent&)
{
    if (onClick)
        onClick();
}

// ─── QualityMonitorCanvas ──────────────────────────────────────────────────────

QualityMonitorCanvas::QualityMonitorCanvas (QualityMonitor* proc)
    : processor (proc)
{
    rmsPanel = std::make_unique<RmsHeatmapPanel>();
    specPanel = std::make_unique<PowerSpectrumPanel>();
    snapPanel = std::make_unique<DataSnapshotPanel>();
    spikePanel = std::make_unique<SpikeRatePanel>();
    addAndMakeVisible (rmsPanel.get());
    addAndMakeVisible (specPanel.get());
    addAndMakeVisible (snapPanel.get());
    addAndMakeVisible (spikePanel.get());

    durationCombo = std::make_unique<ComboBox>();
    durationCombo->addItem ("10 s", 1);
    durationCombo->addItem ("30 s", 2);
    durationCombo->addItem ("60 s", 3);
    durationCombo->setSelectedId (2);
    addAndMakeVisible (durationCombo.get());

    captureBtn = std::make_unique<TextButton> ("Capture");
    captureBtn->setColour (TextButton::buttonColourId, Colour (0xff1976d2));
    addAndMakeVisible (captureBtn.get());

    saveBtn = std::make_unique<TextButton> ("Save");
    saveBtn->setColour (TextButton::buttonColourId, Colour (0xff388e3c));
    addAndMakeVisible (saveBtn.get());

    statusIndicator = std::make_unique<Label>();
    statusIndicator->setText ("RUNNING", dontSendNotification);
    statusIndicator->setColour (Label::textColourId, Colour (0xff4caf50));
    statusIndicator->setFont (Font (11.0f, Font::bold));
    addAndMakeVisible (statusIndicator.get());

    startTimerHz (REFRESH_HZ);
}

QualityMonitorCanvas::~QualityMonitorCanvas() { stopTimer(); }

void QualityMonitorCanvas::refreshState() { updateSettings(); }

void QualityMonitorCanvas::updateSettings()
{
    processor->copyMetricsTo (localMetrics);
    rebuildProbeRows();
    for (int i = 0; i < localMetrics.size(); ++i)
        probeRows[i]->setMetrics (localMetrics[i], i == selectedProbe);
    refresh();
}

void QualityMonitorCanvas::refresh()
{
    processor->copyMetricsTo (localMetrics);
    if (selectedProbe >= localMetrics.size())
        return;
    const auto& m = localMetrics[selectedProbe];
    rmsPanel->updateData (m);
    specPanel->updateData (m);
    snapPanel->updateData (m);
    spikePanel->updateData (m);
    repaint (0, 0, SIDEBAR_W, getHeight());
}

void QualityMonitorCanvas::rebuildProbeRows()
{
    probeRows.clear();
    for (int i = 0; i < localMetrics.size(); ++i)
    {
        auto* row = probeRows.add (new ProbeListRow());
        addAndMakeVisible (row);
        row->onClick = [this, i]
        { selectProbe (i); };
    }
    if (selectedProbe >= localMetrics.size())
        selectedProbe = 0;
    layoutPanels();
}

void QualityMonitorCanvas::selectProbe (int idx)
{
    selectedProbe = idx;
    for (int i = 0; i < probeRows.size(); ++i)
        probeRows[i]->setMetrics (localMetrics[i], i == idx);
    refresh();
}

void QualityMonitorCanvas::resized() { layoutPanels(); }

void QualityMonitorCanvas::layoutPanels()
{
    auto b = getLocalBounds();
    b.removeFromTop (HEADER_H);
    auto sb = b.removeFromLeft (SIDEBAR_W);
    sb.removeFromTop (22);
    for (auto* row : probeRows)
        row->setBounds (sb.removeFromTop (ROW_H));

    auto hdr = getLocalBounds().removeFromTop (HEADER_H).reduced (4, 3);
    durationCombo->setBounds (hdr.removeFromLeft (80));
    hdr.removeFromLeft (8);
    captureBtn->setBounds (hdr.removeFromLeft (72));
    hdr.removeFromLeft (4);
    saveBtn->setBounds (hdr.removeFromLeft (60));
    statusIndicator->setBounds (hdr.removeFromRight (90));

    auto top = b.removeFromTop (b.getHeight() / 2);
    rmsPanel->setBounds (top.removeFromLeft (top.getWidth() / 2));
    specPanel->setBounds (top);
    snapPanel->setBounds (b.removeFromLeft (b.getWidth() / 2));
    spikePanel->setBounds (b);
}

void QualityMonitorCanvas::paint (Graphics& g)
{
    g.fillAll (Colour (0xff111111));
    paintSidebar (g);
    paintHeader (g);
}

void QualityMonitorCanvas::paintSidebar (Graphics& g)
{
    g.setColour (Colour (0xff1e1e1e));
    g.fillRect (0, 0, SIDEBAR_W, getHeight());
    g.setColour (Colours::black);
    g.fillRect (SIDEBAR_W - 1, 0, 1, getHeight());
    g.setColour (Colours::grey);
    g.setFont (Font (10.5f, Font::bold));
    g.drawText ("PROBES", 10, HEADER_H + 4, 100, 16, Justification::centredLeft);

    if (! localMetrics.isEmpty())
    {
        int pass = 0, warn = 0, fail = 0;
        for (const auto& m : localMetrics)
        {
            if (m.status == ProbeStatus::PASS)
                pass++;
            else if (m.status == ProbeStatus::WARN)
                warn++;
            else if (m.status == ProbeStatus::FAIL)
                fail++;
        }
        g.setColour (Colours::grey);
        g.setFont (Font (10.0f));
        g.drawText (String (pass) + " pass  \xc2\xb7  " + String (warn) + " warn  \xc2\xb7  " + String (fail) + " fail",
                    6,
                    HEADER_H + 22 + probeRows.size() * ROW_H + 6,
                    SIDEBAR_W - 12,
                    14,
                    Justification::centredLeft);
    }
}

void QualityMonitorCanvas::paintHeader (Graphics& g)
{
    g.setColour (Colour (0xff1a1a1a));
    g.fillRect (SIDEBAR_W, 0, getWidth() - SIDEBAR_W, HEADER_H);
    g.setColour (Colours::black);
    g.fillRect (SIDEBAR_W, HEADER_H - 1, getWidth() - SIDEBAR_W, 1);
    int midX = SIDEBAR_W + (getWidth() - SIDEBAR_W) / 2;
    int midY = HEADER_H + (getHeight() - HEADER_H) / 2;
    g.setColour (Colours::black.withAlpha (0.8f));
    g.drawVerticalLine (midX, HEADER_H, getHeight());
    g.drawHorizontalLine (midY, SIDEBAR_W, getWidth());
}
