/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
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

#include "QualityMonitorCanvas.h"

#include "QualityMonitor.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include "ColorMap.h"

// ─── QCColours ───────────────────────────────────────────────────────────────

namespace QCColours
{
Colour statusCol (ProbeStatus s)
{
    switch (s)
    {
        case ProbeStatus::PASS: return Colour (0xff4caf50);
        case ProbeStatus::WARN: return Colour (0xffff9800);
        case ProbeStatus::FAIL: return Colour (0xfff44336);
        default:                return Colours::grey;
    }
}

String statusStr (ProbeStatus s)
{
    switch (s)
    {
        case ProbeStatus::PASS: return "PASS";
        case ProbeStatus::WARN: return "WARN";
        case ProbeStatus::FAIL: return "FAIL";
        default:                return "...";
    }
}
} // namespace QCColours

// ─── Shared drawing helpers ───────────────────────────────────────────────────

static constexpr int TITLE_H  = 20;
static constexpr int META_H   = 18;
static constexpr int PLOT_PAD = 10;
static constexpr int AXIS_L   = 38;   // left axis label width
static constexpr int AXIS_B   = 18;   // bottom axis label height
static constexpr int CBAR_W   = 14;

static void drawBadge (Graphics& g, Rectangle<int> r, Colour bg, const String& text)
{
    g.setColour (bg);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);
    g.setColour (Colours::white);
    g.setFont (Font (FontOptions ("Inter", "Semi Bold", 10.0f)));
    g.drawText (text, r, Justification::centred, false);
}

// Font helpers — Inter typeface, sized dynamically by each caller
static Font interRegular  (float size) { return Font (FontOptions ("Inter", "Regular",   size)); }
static Font interSemiBold (float size) { return Font (FontOptions ("Inter", "Semi Bold", size)); }

// Returns a point size scaled to the component height h, clamped to [minSz, maxSz].
static float fscale (int h, float fraction, float minSz, float maxSz)
{
    return jlimit (minSz, maxSz, float (h) * fraction);
}

// Draws Y-axis channel ticks.
// tickCol  — caller-supplied colour (use defaultText.withAlpha for secondary elements)
// fontSize — caller-computed dynamic size
static void drawChannelYTicks (Graphics& g, Rectangle<int> pb, int numCh,
                                const std::initializer_list<int>& ticks,
                                Colour tickCol, float fontSize)
{
    g.setColour (tickCol);
    g.setFont (interRegular (fontSize));
    for (int t : ticks)
    {
        if (t >= numCh)
            break;
        float y = float (pb.getY()) + (float (t) / float (numCh)) * float (pb.getHeight());
        g.drawText (String (t), pb.getX() - AXIS_L, int (y) - 5, AXIS_L - 3, 10, Justification::centredRight);
        g.drawHorizontalLine (int (y), float (pb.getX()), float (pb.getX()) + 4.0f);
    }
}

// ─── RmsHeatmapPanel ─────────────────────────────────────────────────────────
//   Y axis = channels, X axis = RMS value (horizontal colour bars)

void RmsHeatmapPanel::updateData (const ProbeMetrics& m)
{
    rmsUV     = m.rmsUV;
    numCh     = m.numChannels;
    threshUV  = m.rmsThresholdUV;
    numHighRms = m.numHighRmsChannels;
    maxRms    = rmsUV.empty() ? 1.0f : *std::max_element (rmsUV.begin(), rmsUV.end());
    maxRms    = std::max (maxRms, 1.0f);
    repaint();
}

void RmsHeatmapPanel::drawColourBar (Graphics& g, Rectangle<float> r)
{
    for (int y = 0; y < int (r.getHeight()); ++y)
    {
        float t = 1.0f - float (y) / r.getHeight();
        g.setColour (ColourMaps::inferno (t));
        g.fillRect (r.getX(), r.getY() + float (y), r.getWidth(), 1.0f);
    }
    g.setColour (Colours::grey);
    g.setFont (interRegular (8.0f));
    g.drawText ("Hi", int (r.getX()), int (r.getY()),           int (r.getWidth()) + 6, 10, Justification::centredLeft);
    g.drawText ("Lo", int (r.getX()), int (r.getBottom()) - 10, int (r.getWidth()) + 6, 10, Justification::centredLeft);
}

void RmsHeatmapPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentParentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes based on component height
    const int   H       = getHeight();
    const float titleSz = fscale (H, 0.044f, 11.0f, 18.0f);
    const float metaSz  = fscale (H, 0.034f,  9.0f, 14.0f);
    const float tickSz  = fscale (H, 0.028f,  8.0f, 11.0f);
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.6f);

    // Title
    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("RMS Heatmap", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    // Meta row: threshold + warning badge
    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    g.drawText ("THRESHOLD  " + String (threshUV, 0) + " μV", metaRow.removeFromLeft (160), Justification::centredLeft);
    if (numHighRms > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffc62828),
                   String (numHighRms) + " ch above " + String (threshUV, 0) + " μV threshold");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (rmsUV.empty())
        return;

    // Colour bar on right
    auto cbarBounds = b.removeFromRight (CBAR_W + 6).toFloat().reduced (0, 4);
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);
    auto pb = b;

    // Horizontal bars — one per channel, Y = channel index, X width = RMS value
    float rowH = std::max (1.0f, float (pb.getHeight()) / float (numCh));
    for (int c = 0; c < numCh; ++c)
    {
        float t    = rmsUV[c] / maxRms;
        float barW = jlimit (0.0f, float (pb.getWidth()), t * float (pb.getWidth()));
        float y    = float (pb.getY()) + float (c) * rowH;
        g.setColour (ColourMaps::inferno (t));
        g.fillRect (float (pb.getX()), y, barW, rowH);
    }

    // Vertical grid lines
    g.setColour (textCol.withAlpha (0.05f));
    for (int i = 1; i <= 4; ++i)
    {
        float x = float (pb.getX()) + float (i) / 4.0f * float (pb.getWidth());
        g.drawVerticalLine (int (x), float (pb.getY()), float (pb.getBottom()));
    }

    // Threshold line
    float tx = float (pb.getX()) + jlimit (0.0f, 1.0f, threshUV / maxRms) * float (pb.getWidth());
    g.setColour (Colours::white.withAlpha (0.65f));
    g.drawVerticalLine (int (tx), float (pb.getY()), float (pb.getBottom()));
    g.setColour (Colours::white.withAlpha (0.75f));
    g.setFont (interRegular (tickSz));
    g.drawText (String (threshUV, 0), int (tx) + 2, pb.getY() + 2, 30, 10, Justification::centredLeft);

    // X axis ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = float (pb.getX()) + frac * float (pb.getWidth());
        g.drawVerticalLine (int (x), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (int (frac * maxRms)), int (x) - 14, pb.getBottom() + 3, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("RMS (μV)", pb.getX(), pb.getBottom(), pb.getWidth(), AXIS_B, Justification::centred);

    // Y axis ticks (channels)
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);
    g.setFont (interRegular (tickSz));
    g.drawText ("Ch", pb.getX() - AXIS_L, pb.getY() - 11, AXIS_L - 3, 10, Justification::centredRight);

    drawColourBar (g, cbarBounds);
}

// ─── PowerSpectrumPanel ───────────────────────────────────────────────────────

void PowerSpectrumPanel::updateData (const ProbeMetrics& m)
{
    spectrum    = m.powerSpectrum;
    numCh       = m.numChannels;
    sampleRate  = m.sampleRate;
    powerlineHz = m.powerlineHz;
    numNoisyCh  = m.numNoisyChannels;

    // Compute global dB range across all channels (skip DC bin 0)
    float gMin_ = 1e20f, gMax_ = -1e20f;
    for (int c = 0; c < numCh && ! spectrum.empty(); ++c)
    {
        const float* row = spectrum.data() + c * FFT_BINS;
        for (int k = 1; k < FFT_BINS; ++k)
        {
            if (row[k] > 0.0f)
            {
                float db = 10.0f * std::log10 (row[k]);
                gMin_ = std::min (gMin_, db);
                gMax_ = std::max (gMax_, db);
            }
        }
    }
    gMinDb = (gMin_ < 1e19f) ? gMin_ : -120.0f;
    gMaxDb = (gMax_ > -1e19f) ? gMax_ : 0.0f;
    if (gMaxDb <= gMinDb)
        gMaxDb = gMinDb + 1.0f;

    repaint();
}

void PowerSpectrumPanel::drawColourBar (Graphics& g, Rectangle<float> r)
{
    for (int y = 0; y < int (r.getHeight()); ++y)
    {
        float t = 1.0f - float (y) / r.getHeight();
        g.setColour (ColourMaps::viridis (t));
        g.fillRect (r.getX(), r.getY() + float (y), r.getWidth(), 1.0f);
    }
    g.setColour (Colours::grey);
    g.setFont (interRegular (8.0f));
    g.drawText (String (int (gMaxDb)) + "dB", int (r.getX()), int (r.getY()),           int (r.getWidth()) + 32, 10, Justification::centredLeft);
    g.drawText (String (int (gMinDb)) + "dB", int (r.getX()), int (r.getBottom()) - 10, int (r.getWidth()) + 32, 10, Justification::centredLeft);
}

void PowerSpectrumPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentParentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const int   H       = getHeight();
    const float titleSz = fscale (H, 0.044f, 11.0f, 18.0f);
    const float metaSz  = fscale (H, 0.034f,  9.0f, 14.0f);
    const float tickSz  = fscale (H, 0.028f,  8.0f, 11.0f);
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.6f);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Power Spectrum", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    g.drawText ("POWERLINE NOISE  " + String (powerlineHz, 0) + " Hz", metaRow.removeFromLeft (200), Justification::centredLeft);
    if (numNoisyCh > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffe65100), String (numNoisyCh) + " ch noisy");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (spectrum.empty())
        return;

    // Colour bar on right (extra width for dB labels)
    auto cbarBounds = b.removeFromRight (CBAR_W + 32).toFloat().reduced (0, 4);
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);
    auto pb = b;

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    const float nyquist = sampleRate / 2.0f;
    const float dbRange = gMaxDb - gMinDb;

    // Heatmap: render into an Image (one pixel per display pixel), then blit.
    // Each pixel's channel is determined by its Y coordinate; frequency bin by X.
    {
        Image heatmap (Image::ARGB, pw_i, ph_i, true, SoftwareImageType());
        Image::BitmapData bmd (heatmap, Image::BitmapData::writeOnly);

        for (int y = 0; y < ph_i; ++y)
        {
            const int c = jlimit (0, numCh - 1, y * numCh / ph_i);
            const float* row = spectrum.data() + c * FFT_BINS;
            for (int x = 0; x < pw_i; ++x)
            {
                const int   k  = jlimit (1, FFT_BINS - 1, 1 + x * (FFT_BINS - 2) / pw_i);
                const float db = row[k] > 0.0f ? 10.0f * std::log10 (row[k]) : gMinDb;
                const float t  = jlimit (0.0f, 1.0f, (db - gMinDb) / dbRange);
                bmd.setPixelColour (x, y, ColourMaps::viridis (t));
            }
        }

        g.drawImageAt (heatmap, pb.getX(), pb.getY());
    }

    // Powerline harmonic markers (vertical lines)
    const float harmonics[] = { powerlineHz, powerlineHz * 2.0f, 50.0f, 100.0f };
    for (float h : harmonics)
    {
        if (h <= 0.0f || h > nyquist)
            continue;
        float x = float (pb.getX()) + (h / nyquist) * float (pw_i);
        g.setColour (Colour (0xffff9800).withAlpha (0.75f));
        g.drawVerticalLine (int (x), float (pb.getY()), float (pb.getBottom()));
        g.setColour (Colour (0xffff9800));
        g.setFont (interRegular (tickSz));
        g.drawText (String (int (h)) + " Hz", int (x) + 2, pb.getY() + 2, 38, 11, Justification::centredLeft);
    }

    // X axis: frequency ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (float ft : { 0.0f, 50.0f, 100.0f, 150.0f, 200.0f, 300.0f, 500.0f })
    {
        if (ft > nyquist)
            break;
        float x = float (pb.getX()) + (ft / nyquist) * float (pw_i);
        g.drawVerticalLine (int (x), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (int (ft)), int (x) - 15, pb.getBottom() + 4, 30, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Frequency (Hz)", pb.getX(), pb.getBottom(), pw_i, AXIS_B, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);
    g.setFont (interRegular (tickSz));
    g.drawText ("Ch", pb.getX() - AXIS_L, pb.getY() - 11, AXIS_L - 3, 10, Justification::centredRight);

    drawColourBar (g, cbarBounds);
}

// ─── DataSnapshotPanel ────────────────────────────────────────────────────────
//   Y axis = channels (row per channel), X axis = time samples (columns)

void DataSnapshotPanel::updateData (const ProbeMetrics& m)
{
    snapshot = m.dataSnapshot;
    numCh    = m.numChannels;
    if (! snapshot.empty())
    {
        std::vector<float> absV (snapshot.size());
        std::transform (snapshot.begin(), snapshot.end(), absV.begin(), [] (float v) { return std::abs (v); });
        std::sort (absV.begin(), absV.end());
        maxUV = std::max (absV[size_t (0.95f * float (absV.size()))], 1.0f);
        minUV = -maxUV;
    }
    repaint();
}

void DataSnapshotPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentParentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const int   H       = getHeight();
    const float titleSz = fscale (H, 0.044f, 11.0f, 18.0f);
    const float metaSz  = fscale (H, 0.034f,  9.0f, 14.0f);
    const float tickSz  = fscale (H, 0.028f,  8.0f, 11.0f);
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.6f);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Data Snapshot", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    g.drawText ("Color Map  Cividis", b.removeFromTop (META_H), Justification::centredLeft);

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (snapshot.empty())
        return;

    // Colour bar on right
    auto cbar = b.removeFromRight (CBAR_W + 6).toFloat().reduced (0, 4);
    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);
    auto pb = b;

    float range = maxUV - minUV;
    float rowH  = float (pb.getHeight()) / float (numCh);
    float colW  = float (pb.getWidth())  / float (SNAPSHOT_SAMPLES);

    for (int c = 0; c < numCh; ++c)
    {
        const float* row = snapshot.data() + c * SNAPSHOT_SAMPLES;
        float y = float (pb.getY()) + float (c) * rowH;
        for (int s = 0; s < SNAPSHOT_SAMPLES; ++s)
        {
            g.setColour (ColourMaps::cividis (jlimit (0.0f, 1.0f, (row[s] - minUV) / range)));
            g.fillRect (float (pb.getX()) + float (s) * colW, y,
                        std::max (colW, 1.0f), std::max (rowH, 1.0f));
        }
    }

    // Colour bar
    for (int y = 0; y < int (cbar.getHeight()); ++y)
    {
        g.setColour (ColourMaps::cividis (1.0f - float (y) / cbar.getHeight()));
        g.fillRect (cbar.getX(), cbar.getY() + float (y), cbar.getWidth(), 1.0f);
    }
    g.setColour (Colours::grey);
    g.setFont (interRegular (8.0f));
    g.drawText ("+" + String (int (maxUV)) + "\xc2\xb5V", int (cbar.getX()), int (cbar.getY()),           int (cbar.getWidth()) + 6, 10, Justification::centredLeft);
    g.drawText ("-" + String (int (maxUV)) + "\xc2\xb5V", int (cbar.getX()), int (cbar.getBottom()) - 10, int (cbar.getWidth()) + 6, 10, Justification::centredLeft);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);
    g.setFont (interRegular (tickSz));
    g.drawText ("Ch", pb.getX() - AXIS_L, pb.getY() - 11, AXIS_L - 3, 10, Justification::centredRight);

    // X axis: time sample ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = float (pb.getX()) + frac * float (pb.getWidth());
        int   samp = int (frac * float (SNAPSHOT_SAMPLES));
        g.drawVerticalLine (int (x), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (samp), int (x) - 14, pb.getBottom() + 3, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (samples)", pb.getX(), pb.getBottom(), pb.getWidth(), AXIS_B, Justification::centred);
}

// ─── SpikeRatePanel ───────────────────────────────────────────────────────────
//   Y axis = channels, X axis = spike rate Hz (horizontal bars)

void SpikeRatePanel::updateData (const ProbeMetrics& m)
{
    rateHz     = m.spikeRateHz;
    numCh      = m.numChannels;
    spikeFailHz = m.spikeRateFailHz;
    spikeLowHz  = m.spikeRateLowHz;
    numLowCh   = m.numLowSpikeChannels;
    maxRateHz  = rateHz.empty() ? 30.0f : *std::max_element (rateHz.begin(), rateHz.end());
    maxRateHz  = std::max (maxRateHz, spikeLowHz * 2.0f);
    repaint();
}

void SpikeRatePanel::drawLegend (Graphics& g, Rectangle<int> r)
{
    struct { Colour col; const char* lbl; } e[] = {
        { Colour (0xff42a5f5), "Normal (>=2 Hz)"  },
        { Colour (0xffff9800), "Low (0.1-2 Hz)"   },
        { Colour (0xfff44336), "Fail (<0.1 Hz)"   }
    };
    const float legendFs = fscale (getHeight(), 0.028f, 8.0f, 10.0f);
    int y = r.getY() + 2;
    for (auto& v : e)
    {
        g.setColour (v.col);
        g.fillRect (r.getX(), y, 10, 10);
        g.setColour (findColour (ThemeColours::defaultText));
        g.setFont (interRegular (legendFs));
        g.drawText (v.lbl, r.getX() + 13, y, r.getWidth() - 13, 10, Justification::centredLeft);
        y += 13;
    }
}

void SpikeRatePanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentParentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const int   H       = getHeight();
    const float titleSz = fscale (H, 0.044f, 11.0f, 18.0f);
    const float metaSz  = fscale (H, 0.034f,  9.0f, 14.0f);
    const float tickSz  = fscale (H, 0.028f,  8.0f, 11.0f);
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.6f);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Spike Rate", b.removeFromTop (TITLE_H).toFloat().reduced (4, 0), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    g.drawText ("SPIKE THRESH  " + String (spikeFailHz, 1) + " Hz", metaRow.removeFromLeft (180), Justification::centredLeft);
    if (numLowCh > 0)
        drawBadge (g, metaRow.reduced (2, 1), Colour (0xffc62828),
                   String (numLowCh) + " ch below " + String (spikeFailHz, 1) + " Hz threshold");

    b.reduce (PLOT_PAD, PLOT_PAD);
    if (rateHz.empty())
        return;

    // Legend top-right
    drawLegend (g, b.removeFromTop (44).removeFromRight (130));

    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);
    auto pb = b;
    float px = float (pb.getX()), py = float (pb.getY());
    float pw = float (pb.getWidth()), ph = float (pb.getHeight());

    // Vertical grid lines
    g.setColour (textCol.withAlpha (0.05f));
    for (int i = 1; i <= 4; ++i)
    {
        float x = px + float (i) / 4.0f * pw;
        g.drawVerticalLine (int (x), py, py + ph);
    }

    // Threshold vertical line
    float tx = px + jlimit (0.0f, 1.0f, spikeFailHz / maxRateHz) * pw;
    g.setColour (Colour (0xffc62828).withAlpha (0.75f));
    g.drawVerticalLine (int (tx), py, py + ph);
    g.setColour (Colour (0xffff5252));
    g.setFont (interRegular (tickSz));
    g.drawText (String (spikeFailHz, 1) + " Hz", int (tx) + 2, int (py) + 2, 40, 10, Justification::centredLeft);

    // Horizontal bars per channel
    float rowH = std::max (1.0f, ph / float (numCh));
    for (int c = 0; c < numCh; ++c)
    {
        float rate = rateHz[c];
        float barW = jlimit (0.0f, pw, (rate / maxRateHz) * pw);
        float y    = py + float (c) * rowH;
        g.setColour (rate < spikeFailHz ? Colour (0xfff44336)
                   : rate < spikeLowHz  ? Colour (0xffff9800)
                                        : Colour (0xff42a5f5));
        g.fillRect (px, y, barW, rowH);
    }

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);
    g.setFont (interRegular (tickSz));
    g.drawText ("Ch", pb.getX() - AXIS_L, pb.getY() - 11, AXIS_L - 3, 10, Justification::centredRight);

    // X axis ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = px + frac * pw;
        g.drawVerticalLine (int (x), py + ph, py + ph + 4.0f);
        g.drawText (String (frac * maxRateHz, 1, false), int (x) - 16, int (py + ph + 4), 34, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Spike Rate (Hz)", int (px), int (py + ph + 4), int (pw), AXIS_B, Justification::centred);
}

// ─── ProbeListModel ──────────────────────────────────────────────────────────

ProbeListModel::ProbeListModel (QualityMonitorCanvas* c)
    : parent (c)
{
}

void ProbeListModel::setMetrics (const Array<ProbeMetrics>& metrics, int selected)
{
    localMetrics = metrics;
    selectedIdx  = selected;
}

int ProbeListModel::getNumRows()
{
    return localMetrics.size();
}

void ProbeListModel::paintListBoxItem (int row, Graphics& g, int width, int height, bool rowIsSelected)
{
    if (row < 0 || row >= localMetrics.size())
        return;

    const auto& m  = localMetrics[row];
    auto         b  = Rectangle<int> (0, 0, width, height);
    Colour       sc = m.status == ProbeStatus::UNKNOWN ? parent->findColour (ThemeColours::defaultFill) : QCColours::statusCol (m.status);

    // Row background
    if (rowIsSelected)
    {
        g.setColour (sc.withAlpha (0.15f));
        g.fillRoundedRectangle (b.toFloat().reduced (3, 2), 5.0f);
        g.setColour (sc.withAlpha (0.60f));
        g.drawRoundedRectangle (b.toFloat().reduced (3, 2), 5.0f, 1.5f);
    }

    // Status dot
    g.setColour (sc);
    g.fillEllipse (9.0f, float (b.getCentreY()) - 5.0f, 10.0f, 10.0f);

    // Name
    String name = m.streamName.isEmpty() ? "Probe" : m.streamName;
    Colour textCol = parent->findColour (ThemeColours::defaultText);
    g.setColour (rowIsSelected ? textCol : textCol.withAlpha (0.6f));
    g.setFont (Font (12.0f, Font::bold));
    g.drawText (name, 26, b.getY() + 6, width - 80, 15, Justification::centredLeft);

    // Channel count
    g.setColour (rowIsSelected ? textCol : textCol.withAlpha (0.6f));
    g.setFont (Font (10.0f));
    g.drawText (String (m.numChannels) + " ch", 26, b.getY() + 22, width - 80, 13, Justification::centredLeft);

    // Status badge
    drawBadge (g, b.removeFromRight (50).reduced (6, 10), sc, QCColours::statusStr (m.status));
}

void ProbeListModel::selectedRowsChanged (int lastRowSelected)
{
    if (onProbeSelected && lastRowSelected >= 0)
        onProbeSelected (lastRowSelected);
}

// ─── ContentComponent ────────────────────────────────────────────────────────

ContentComponent::ContentComponent()
{
    rmsPanel   = std::make_unique<RmsHeatmapPanel>();
    specPanel  = std::make_unique<PowerSpectrumPanel>();
    snapPanel  = std::make_unique<DataSnapshotPanel>();
    spikePanel = std::make_unique<SpikeRatePanel>();
    addAndMakeVisible (rmsPanel.get());
    addAndMakeVisible (specPanel.get());
    addAndMakeVisible (snapPanel.get());
    addAndMakeVisible (spikePanel.get());
}

void ContentComponent::resized()
{
    auto b = getLocalBounds();
    auto top = b.removeFromTop (b.getHeight() / 2);
    rmsPanel->setBounds  (top.removeFromLeft (top.getWidth() / 2));
    specPanel->setBounds (top);
    snapPanel->setBounds (b.removeFromLeft (b.getWidth() / 2));
    spikePanel->setBounds (b);
}

// ─── QualityMonitorCanvas ────────────────────────────────────────────────────

QualityMonitorCanvas::QualityMonitorCanvas (QualityMonitor* proc)
    : processor (proc)
{
    // Probe list sidebar
    probeListModel = std::make_unique<ProbeListModel>(this);
    probeListModel->onProbeSelected = [this] (int idx) { selectProbe (idx); };

    probeListBox = std::make_unique<ListBox> ("ProbeList", probeListModel.get());
    probeListBox->setRowHeight (52);
    probeListBox->setMultipleSelectionEnabled (false);
    // probeListBox->setColour (ListBox::backgroundColourId,     Colour (0));
    // probeListBox->setColour (ListBox::outlineColourId,        Colour (0));
    // probeListBox->setColour (ListBox::textColourId,           Colours::white);
    addAndMakeVisible (probeListBox.get());

    // Scrollable content with four panels
    content  = std::make_unique<ContentComponent>();
    viewport = std::make_unique<Viewport>();
    viewport->setViewedComponent (content.get(), false);
    viewport->setScrollBarsShown (true, true);
    viewport->setScrollBarThickness (8);
    addAndMakeVisible (viewport.get());

    // Header controls
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

    refreshRate = 5.0f;
}

QualityMonitorCanvas::~QualityMonitorCanvas() { stopTimer(); }

void QualityMonitorCanvas::refreshState() { updateSettings(); }

void QualityMonitorCanvas::updateSettings()
{
    processor->copyMetricsTo (localMetrics);
    probeListModel->setMetrics (localMetrics, selectedProbe);
    probeListBox->updateContent();
    if (selectedProbe < localMetrics.size())
        probeListBox->selectRow (selectedProbe, false, true);
    refresh();
}

void QualityMonitorCanvas::refresh()
{
    processor->copyMetricsTo (localMetrics);

    // Update the sidebar list with latest status colours
    probeListModel->setMetrics (localMetrics, selectedProbe);
    probeListBox->repaintRow (selectedProbe);
    for (int i = 0; i < localMetrics.size(); ++i)
        probeListBox->repaintRow (i);

    if (selectedProbe >= localMetrics.size())
        return;

    const auto& m = localMetrics[selectedProbe];
    content->rmsPanel->updateData (m);
    content->specPanel->updateData (m);
    content->snapPanel->updateData (m);
    content->spikePanel->updateData (m);
}

void QualityMonitorCanvas::selectProbe (int idx)
{
    selectedProbe = idx;
    probeListModel->setMetrics (localMetrics, idx);
    probeListBox->selectRow (idx, false, true);
    refresh();
}

void QualityMonitorCanvas::resized() { layoutPanels(); }

void QualityMonitorCanvas::layoutPanels()
{
    auto b = getLocalBounds();

    // Header strip
    auto hdr = b.removeFromTop (HEADER_H).reduced (4, 3);
    durationCombo->setBounds (hdr.removeFromLeft (80));
    hdr.removeFromLeft (8);
    captureBtn->setBounds (hdr.removeFromLeft (72));
    hdr.removeFromLeft (4);
    saveBtn->setBounds (hdr.removeFromLeft (60));
    statusIndicator->setBounds (hdr.removeFromRight (90));

    // Sidebar
    auto sb = b.removeFromLeft (SIDEBAR_W);
    // "PROBES" label occupies top 22 px; ListBox fills the rest
    sb.removeFromTop (30);
    sb.setHeight (probeListBox->getRowHeight() * probeListModel->getNumRows() + 2);
    probeListBox->setBounds (sb.reduced (1, 0));

    // Viewport fills remaining area; content is sized to at least the viewport
    viewport->setBounds (b);
    int cw = std::max (b.getWidth(),  2 * ContentComponent::MIN_PANEL_W);
    int ch = std::max (b.getHeight(), 2 * ContentComponent::MIN_PANEL_H);
    content->setSize (cw, ch);
}

void QualityMonitorCanvas::paint (Graphics& g)
{
    g.fillAll (findColour (ThemeColours::windowBackground));

    // Header background
    g.setColour (findColour (ThemeColours::componentBackground));
    g.fillRect (0, 0, getWidth(), HEADER_H);
    g.setColour (findColour (ThemeColours::outline));
    g.fillRect (1, HEADER_H - 1, getWidth() - 2, 1);

    // Sidebar background
    g.setColour (findColour (ThemeColours::componentBackground));
    g.fillRect (0, HEADER_H, SIDEBAR_W, getHeight() - HEADER_H);
    g.setColour (findColour (ThemeColours::outline));
    g.fillRect (SIDEBAR_W - 1, HEADER_H, 1, getHeight() - HEADER_H);

    // "PROBES" label in sidebar
    g.setColour (findColour (ThemeColours::defaultFill));
    g.fillRect (0, HEADER_H, SIDEBAR_W - 1, 30);
    g.setColour (findColour (ThemeColours::defaultText));
    g.setFont (FontOptions ("Inter", "Semi Bold", 14.0f));
    g.drawText ("DATA STREAMS", 10, HEADER_H, SIDEBAR_W - 12, 30, Justification::centredLeft);

    // Panel dividers inside content area (cosmetic — mirrored at viewport level)
    // (The ContentComponent's own resized() layout handles panel borders)
}
