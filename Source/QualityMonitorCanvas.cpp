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

static constexpr int TITLE_H  = 22;
static constexpr int META_H   = 20;
static constexpr int PLOT_PAD = 10;
static constexpr int AXIS_L   = 36;   // left axis label width + tick mark length
static constexpr int AXIS_B   = 30;   // bottom: tick labels (14 px) + axis title (14 px)
static constexpr int CBAR_W   = 14;
static constexpr int STRIP_W  = 48;   // per-channel live bar strip width

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
        g.drawText (String (t), pb.getX() - AXIS_L, int (y) - 5, AXIS_L - 6, 10, Justification::centredRight);
        g.drawHorizontalLine (int (y), float (pb.getX()) - 4.0f, float (pb.getX()));
    }
}

// ─── RmsHeatmapPanel ─────────────────────────────────────────────────────────
//   Y axis = channels, X axis = time (one column per RMS window ∼1 s)

void RmsHeatmapPanel::updateData (const ProbeMetrics& m)
{
    rmsUV               = m.rmsUV;
    rmsHistory          = m.rmsHistory;
    rmsHistoryFrames    = m.rmsHistoryFrames;
    rmsHistoryMaxFrames = std::max (1, m.rmsHistoryMaxFrames);
    durationSec         = std::max (1, m.analysisDurationSec);
    numCh               = m.numChannels;
    threshUV            = m.rmsThresholdUV;
    numHighRms          = m.numHighRmsChannels;
    processingDone      = m.processingDone;

    // maxRms from all accumulated frames (not just latest)
    maxRms = 1.0f;
    const int validSamples = rmsHistoryFrames * numCh;
    for (int i = 0; i < validSamples && i < (int) rmsHistory.size(); ++i)
        maxRms = std::max (maxRms, rmsHistory[i]);

    repaint();
}

void RmsHeatmapPanel::drawColourBar (Graphics& g, Rectangle<float> r)
{
    // Gradient fill
    for (int y = 0; y < int (r.getHeight()); ++y)
    {
        float t = 1.0f - float (y) / r.getHeight();
        g.setColour (ColourMaps::inferno (t));
        g.fillRect (r.getX(), r.getY() + float (y), r.getWidth(), 1.0f);
    }
    // Labels overlaid inside the bar
    g.setFont (interRegular (8.0f));
    g.setColour (Colours::white.withAlpha (0.85f));
    g.drawText (String (int (maxRms)) + "μV",
                int (r.getX()), int (r.getY()),
                int (r.getWidth()), 10, Justification::centred);
    g.drawText ("0",
                int (r.getX()), int (r.getBottom()) - 10,
                int (r.getWidth()), 10, Justification::centred);
}

void RmsHeatmapPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentBackground));

    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    const float titleSz = 16.0f;
    const float metaSz  = 14.0f;
    const float tickSz  =  11.0f;
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.8f);

    b.reduce (PLOT_PAD, PLOT_PAD);

    // Title row
    auto titleRow = b.removeFromTop (TITLE_H);
    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("RMS Heatmap", titleRow, Justification::centredLeft);

    // Meta row: threshold label + alert badge
    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    auto threshLabel = metaRow.removeFromLeft (160);
    g.drawText ("THRESHOLD  " + String (threshUV, 0) + " μV", threshLabel, Justification::centredLeft);
    if (numHighRms > 0)
        drawBadge (g, metaRow.reduced (2, 2), Colour (0xffc62828),
                   String (numHighRms) + " ch above " + String (threshUV, 0) + " μV threshold");

    b.reduce (0, PLOT_PAD);
    if (rmsHistory.empty() || numCh == 0)
        return;
        
    b.removeFromBottom (AXIS_B);
    
    // Colour bar on right
    auto cbarBounds = b.removeFromRight (CBAR_W + 4).toFloat();
    b.removeFromRight (PLOT_PAD);
    // Live per-channel strip (between main plot and gap before colour bar)
    auto stripBounds = b.removeFromRight (STRIP_W);
    b.removeFromRight (PLOT_PAD);
    b.removeFromLeft (AXIS_L);
    auto pb = b;

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // --- Heatmap via BitmapData ---
    {
        Image heatmap (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        heatmap.clear (heatmap.getBounds(), findColour (ThemeColours::defaultFill));
        Image::BitmapData bmd (heatmap, Image::BitmapData::writeOnly);

        // Only paint frames that have been accumulated
        if (rmsHistoryFrames > 0 && maxRms > 0.0f)
        {
            const int filledPx = std::min (pw_i,
                int (int64_t (rmsHistoryFrames) * int64_t (pw_i) / int64_t (rmsHistoryMaxFrames)));

            for (int y = 0; y < ph_i; ++y)
            {
                const int ch = std::min (numCh - 1, std::max (0, y * numCh / ph_i));
                for (int x = 0; x < filledPx; ++x)
                {
                    const int frame = std::min (rmsHistoryFrames - 1, std::max (0,
                        int (int64_t (x) * int64_t (rmsHistoryMaxFrames) / int64_t (pw_i))));
                    const float rms = rmsHistory[frame * numCh + ch];
                    const float t   = std::min (1.0f, std::max (0.0f, rms / maxRms));
                    bmd.setPixelColour (x, y, ColourMaps::inferno (t));
                }
            }
        }

        g.drawImageAt (heatmap, pb.getX(), pb.getY());
    }

    // --- Live per-channel strip (latest RMS, Inferno) ---
    {
        const int sw = stripBounds.getWidth();
        const int sh = ph_i;
        const Colour bg = findColour (ThemeColours::defaultFill);
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), bg);
        if (! rmsUV.empty() && maxRms > 0.0f)
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int ch  = jlimit (0, numCh - 1, y * numCh / sh);
                const float t = jlimit (0.0f, 1.0f, rmsUV[ch] / maxRms);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = ColourMaps::inferno (t);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, stripBounds.getX(), stripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (stripBounds.expanded (1), 1);
    }

    // Threshold colour marker on the colour bar (horizontal line inside cbar)
    {
        const float tY = cbarBounds.getY()
                       + (1.0f - jlimit (0.0f, 1.0f, threshUV / maxRms))
                       * cbarBounds.getHeight();
        g.setColour (Colours::yellow.withAlpha (0.6f));
        g.drawHorizontalLine (int (tY),
                              cbarBounds.getX() - 3.0f,
                              cbarBounds.getRight());
        g.setFont (interRegular (tickSz));
        g.drawText (String (threshUV, 0),
                    int (cbarBounds.getX()) - AXIS_L,
                    int (tY) - 6, AXIS_L - 4, 12,
                    Justification::centredRight);
    }

    // Progress line (white vertical bar at current frame)
    if (! processingDone && rmsHistoryFrames > 0 && rmsHistoryMaxFrames > 0)
    {
        const float progX = float (pb.getX())
                          + float (rmsHistoryFrames) / float (rmsHistoryMaxFrames)
                          * float (pw_i);
        g.setColour (Colours::white.withAlpha (0.6f));
        g.drawVerticalLine (int (progX), float (pb.getY()), float (pb.getBottom()));
    }

    // X axis: time ticks (labels in seconds regardless of frame count)
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        const float frac = float (i) / 4.0f;
        const float x    = float (pb.getX()) + frac * float (pw_i);
        const int   sec  = int (frac * float (durationSec));
        g.drawVerticalLine (int (x) - std::floor (frac), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (sec), int (x) - 14, pb.getBottom() + 4, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (s)", pb.getX(), pb.getBottom() + 16, pw_i, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);

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
    hasData = (gMax_ > -1e19f);

    // Compute mean dB per channel for the live strip
    channelMeanDb.assign (numCh, gMinDb);
    for (int c = 0; c < numCh && ! spectrum.empty(); ++c)
    {
        const float* row = spectrum.data() + c * FFT_BINS;
        float sum = 0.0f;
        int   cnt = 0;
        for (int k = 1; k < FFT_BINS; ++k)
        {
            if (row[k] > 0.0f)
            {
                sum += 10.0f * std::log10 (row[k]);
                ++cnt;
            }
        }
        if (cnt > 0)
            channelMeanDb[c] = sum / float (cnt);
    }

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
    // Labels overlaid inside the bar
    g.setFont (interRegular (8.0f));
    g.setColour (Colours::white.withAlpha (0.85f));
    g.drawText (String (int (gMaxDb)) + "dB",
                int (r.getX()), int (r.getY()),           int (r.getWidth()), 10, Justification::centred);
    g.drawText (String (int (gMinDb)) + "dB",
                int (r.getX()), int (r.getBottom()) - 10, int (r.getWidth()), 10, Justification::centred);
}

void PowerSpectrumPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const float titleSz = 16.0f;
    const float metaSz  = 14.0f;
    const float tickSz  =  11.0f;
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.8f);

    b.reduce (PLOT_PAD, PLOT_PAD);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Power Spectrum", b.removeFromTop (TITLE_H), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    auto plLabel = metaRow.removeFromLeft (210);
    g.drawText ("POWERLINE NOISE  " + String (powerlineHz, 0) + " Hz", plLabel, Justification::centredLeft);
    if (numNoisyCh > 0)
        drawBadge (g, metaRow.reduced (2, 2), Colour (0xffe65100), String (numNoisyCh) + " ch noisy");

    b.reduce (0, PLOT_PAD);
    if (spectrum.empty())
        return;
    
        b.removeFromBottom (AXIS_B);

    // Colour bar on right
    auto cbarBounds = b.removeFromRight (CBAR_W + 4).toFloat();
    b.removeFromRight (PLOT_PAD);
    // Live per-channel strip
    auto stripBounds = b.removeFromRight (STRIP_W);
    b.removeFromLeft (AXIS_L);
    b.removeFromRight (PLOT_PAD);
    auto pb = b;

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    const float nyquist = sampleRate / 2.0f;
    const float dbRange = gMaxDb - gMinDb;

    // Draw border around plot area
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // Heatmap: render into an Image (one pixel per display pixel), then blit.
    // Each pixel's channel is determined by its Y coordinate; frequency bin by X.
    {
        Image heatmap (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        heatmap.clear (heatmap.getBounds(), findColour (ThemeColours::defaultFill));
        if (hasData)
        {
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
        }

        g.drawImageAt (heatmap, pb.getX(), pb.getY());
    }

    // --- Live per-channel strip (mean dB, Viridis) ---
    {
        const int sw = stripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::defaultFill));
        if (! channelMeanDb.empty() && dbRange > 0.0f)
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch  = jlimit (0, numCh - 1, y * numCh / sh);
                const float t   = jlimit (0.0f, 1.0f, (channelMeanDb[ch] - gMinDb) / dbRange);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = ColourMaps::viridis (t);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, stripBounds.getX(), stripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (stripBounds.expanded (1), 1);
    }

    // Powerline harmonic markers: coloured lines only, no text labels (avoid clutter)
    const float harmonics[] = { powerlineHz, powerlineHz * 2.0f, 50.0f, 100.0f };
    for (float h : harmonics)
    {
        if (h <= 0.0f || h > nyquist)
            continue;
        float x = float (pb.getX()) + (h / nyquist) * float (pw_i);
        g.setColour (Colour (0xffff9800).withAlpha (0.55f));
        g.drawVerticalLine (int (x), float (pb.getY()), float (pb.getBottom()));
    }

    // X axis: frequency ticks (skip values if too close together on screen)
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    int lastLabelX = -100;
    for (float ft : { 0.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 15000.0f })
    {
        if (ft > nyquist) break;
        int x = int (float (pb.getX()) + (ft / nyquist) * float (pw_i));
        g.drawVerticalLine (x, float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        if (x - lastLabelX >= 28)
        {
            String lbl = ft >= 1000.0f ? (String (int (ft / 1000.0f)) + "k") : String (int (ft));
            g.drawText (lbl, x - 14, pb.getBottom() + 4, 28, 11, Justification::centred);
            lastLabelX = x;
        }
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Frequency (Hz)", pb.getX(), pb.getBottom() + 16, pw_i, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);

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

    // Compute mean |sample| per channel for the live strip
    channelMeanUV.assign (numCh, 0.0f);
    hasData = false;
    for (int c = 0; c < numCh && ! snapshot.empty(); ++c)
    {
        const float* row = snapshot.data() + c * SNAPSHOT_SAMPLES;
        float sum = 0.0f;
        for (int s = 0; s < SNAPSHOT_SAMPLES; ++s)
        {
            const float av = std::abs (row[s]);
            sum += av;
            if (av > 0.0f) hasData = true;
        }
        channelMeanUV[c] = sum / float (SNAPSHOT_SAMPLES);
    }

    repaint();
}

void DataSnapshotPanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const float titleSz = 16.0f;
    const float metaSz  = 14.0f;
    const float tickSz  =  11.0f;
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.8f);

    b.reduce (PLOT_PAD, PLOT_PAD);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Data Snapshot", b.removeFromTop (TITLE_H).toFloat(), Justification::centredLeft);

    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    g.drawText ("Color Map  Cividis", b.removeFromTop (META_H), Justification::centredLeft);

    b.reduce (0, PLOT_PAD);
    if (snapshot.empty())
        return;

    b.removeFromBottom (AXIS_B);

    // Colour bar on right
    auto cbar = b.removeFromRight (CBAR_W + 4).toFloat();
    b.removeFromRight (PLOT_PAD);
    // Live per-channel strip
    auto stripBounds = b.removeFromRight (STRIP_W);
    b.removeFromLeft (AXIS_L);
    b.removeFromRight (PLOT_PAD);
    auto pb = b;

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    // Draw border around plot area
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    const float range = maxUV - minUV;

    // --- Snapshot via BitmapData ---
    {
        Image snapshotImg (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        snapshotImg.clear (snapshotImg.getBounds(), findColour (ThemeColours::defaultFill));
        if (hasData)
        {
            Image::BitmapData bmd (snapshotImg, Image::BitmapData::writeOnly);

            for (int y = 0; y < ph_i; ++y)
            {
                const int c = jlimit (0, numCh - 1, y * numCh / ph_i);
                const float* row = snapshot.data() + c * SNAPSHOT_SAMPLES;
                for (int x = 0; x < pw_i; ++x)
                {
                    const int   s = jlimit (0, SNAPSHOT_SAMPLES - 1, x * SNAPSHOT_SAMPLES / pw_i);
                    const float t = jlimit (0.0f, 1.0f, (row[s] - minUV) / range);
                    bmd.setPixelColour (x, y, ColourMaps::cividis (t));
                }
            }
        }

        g.drawImageAt (snapshotImg, pb.getX(), pb.getY());
    }

    // --- Live per-channel strip (peak |voltage|, Cividis, 0 → maxUV) ---
    {
        const int sw = stripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::defaultFill));
        if (! channelMeanUV.empty() && maxUV > 0.0f)
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = jlimit (0, numCh - 1, y * numCh / sh);
                const float t    = jlimit (0.0f, 1.0f, channelMeanUV[ch] / maxUV);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = ColourMaps::cividis (t);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, stripBounds.getX(), stripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (stripBounds.expanded (1), 1);
    }

    // Colour bar (labels overlaid inside)
    for (int y = 0; y < int (cbar.getHeight()); ++y)
    {
        g.setColour (ColourMaps::cividis (1.0f - float (y) / cbar.getHeight()));
        g.fillRect (cbar.getX(), cbar.getY() + float (y), cbar.getWidth(), 1.0f);
    }
    g.setFont (interRegular (8.0f));
    g.setColour (Colours::white.withAlpha (0.85f));
    g.drawText ("+" + String (int (maxUV)) + "μV",
                int (cbar.getX()), int (cbar.getY()),           int (cbar.getWidth()), 10, Justification::centred);
    g.drawText ("-" + String (int (maxUV)) + "μV",
                int (cbar.getX()), int (cbar.getBottom()) - 10, int (cbar.getWidth()), 10, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);

    // X axis: time sample ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = float (pb.getX()) + frac * float (pb.getWidth());
        int   samp = int (frac * float (SNAPSHOT_SAMPLES));
        g.drawVerticalLine (int (x), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (samp), int (x) - 14, pb.getBottom() + 4, 28, 11, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (samples)", pb.getX(), pb.getBottom() + 16, pb.getWidth(), 12, Justification::centred);
}

// ─── SpikeRatePanel ───────────────────────────────────────────────────────────
//   Y axis = channels, X axis = spike rate Hz (horizontal bars)

void SpikeRatePanel::updateData (const ProbeMetrics& m)
{
    rateHz      = m.spikeRateHz;
    rateLiveHz  = m.spikeRateLiveHz;
    numCh       = m.numChannels;
    spikeFailHz = m.spikeRateFailHz;
    spikeLowHz  = m.spikeRateLowHz;
    numLowCh    = m.numLowSpikeChannels;
    maxRateHz      = rateHz.empty()     ? 30.0f : *std::max_element (rateHz.begin(),     rateHz.end());
    maxRateHz      = std::max (maxRateHz, spikeLowHz * 2.0f);
    maxLiveRateHz  = rateLiveHz.empty() ? maxRateHz : *std::max_element (rateLiveHz.begin(), rateLiveHz.end());
    maxLiveRateHz  = std::max (maxLiveRateHz, spikeLowHz * 2.0f);
    repaint();
}

void SpikeRatePanel::drawLegend (Graphics& g, Rectangle<int> r)
{
    struct { Colour col; const char* lbl; } e[] = {
        { Colour (0xff42a5f5), "Normal (>=2 Hz)"  },
        { Colour (0xffff9800), "Low (0.1-2 Hz)"   },
        { Colour (0xfff44336), "Fail (<0.1 Hz)"   }
    };
    const float legendFs = 12.0f;
    int y = r.getY() + 4;
    for (auto& v : e)
    {
        g.setColour (v.col);
        g.fillRect (r.getX(), y, 12, 12);
        g.setColour (findColour (ThemeColours::defaultText));
        g.setFont (interRegular (legendFs));
        g.drawText (v.lbl, r.getX() + 15, y, r.getWidth() - 15, 12, Justification::centredLeft);
        y += 15;
    }
}

void SpikeRatePanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

    // Dynamic font sizes
    const float titleSz = 16.0f;
    const float metaSz  = 14.0f;
    const float tickSz  =  11.0f;
    const Colour textCol = findColour (ThemeColours::defaultText);
    const Colour tickCol = textCol.withAlpha (0.8f);

    b.reduce (PLOT_PAD, PLOT_PAD);

    g.setColour (textCol);
    g.setFont (interSemiBold (titleSz));
    g.drawText ("Spike Rate", b.removeFromTop (TITLE_H).toFloat(), Justification::centredLeft);

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    auto spikeLabel = metaRow.removeFromLeft (190);
    g.drawText ("SPIKE THRESH  " + String (spikeFailHz, 1) + " Hz", spikeLabel, Justification::centredLeft);
    if (numLowCh > 0)
        drawBadge (g, metaRow.reduced (2, 2), Colour (0xffc62828),
                   String (numLowCh) + " ch below " + String (spikeFailHz, 1) + " Hz threshold");

    b.reduce (0, PLOT_PAD);
    if (rateHz.empty())
        return;

    b.removeFromLeft (AXIS_L);
    b.removeFromBottom (AXIS_B);
    // Live per-channel strip on the right
    auto stripBounds = b.removeFromRight (STRIP_W);
    b.removeFromRight (PLOT_PAD);
    auto pb = b;
    float px = float (pb.getX()), py = float (pb.getY());
    float pw = float (pb.getWidth()), ph = float (pb.getHeight());

    // Paint background and border around plot area
    g.setColour (findColour (ThemeColours::defaultFill));
    g.fillRect (pb);
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // Vertical grid lines
    g.setColour (textCol.withAlpha (0.05f));
    for (int i = 1; i <= 4; ++i)
    {
        float x = px + float (i) / 4.0f * pw;
        g.drawVerticalLine (int (x), py, py + ph);
    }

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

    // Threshold vertical line
    float tx = px + jlimit (0.0f, 1.0f, spikeFailHz / maxRateHz) * pw;
    g.setColour (Colour (0xffc62828).withAlpha (0.75f));
    g.drawVerticalLine (int (tx), py, py + ph);
    g.setColour (Colour (0xffff5252));
    g.setFont (interRegular (tickSz));
    g.drawText (String (spikeFailHz, 1) + " Hz", int (tx) + 2, int (py) + 2, 40, 10, Justification::centredLeft);

    // --- Live per-channel strip (spike rate, threshold colours) ---
    {
        const int sw   = stripBounds.getWidth();
        const int sh   = int (ph);
        const int sy   = int (py);
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::defaultFill));
        if (! rateLiveHz.empty() && maxLiveRateHz > 0.0f)
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = jlimit (0, numCh - 1, y * numCh / sh);
                const float rate = rateLiveHz[ch];
                const float t    = jlimit (0.0f, 1.0f, rate / maxLiveRateHz);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = rate < spikeFailHz ? Colour (0xfff44336)
                                 : rate < spikeLowHz  ? Colour (0xffff9800)
                                                      : Colour (0xff42a5f5);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, stripBounds.getX(), sy);
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (Rectangle<int> (stripBounds.getX(), sy, sw, sh).expanded (1), 1);
    }

    // Legend top-right
    drawLegend (g, b.removeFromBottom (50).removeFromRight (110));

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, numCh, { 0, 96, 192, 288, 383 }, tickCol, tickSz);

    // X axis ticks
    g.setColour (tickCol);
    g.setFont (interRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = px + frac * pw;
        g.drawVerticalLine (int (x), py + ph, py + ph + 4.0f);
        g.drawText (String (frac * maxRateHz, 1, false), int (x) - 16, int (py + ph + 4), 34, 11, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Spike Rate (Hz)", int (px), int (py + ph + 16), int (pw), 12, Justification::centred);
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

    if (row % 2 == 0)
    {
        g.setColour (parent->findColour (ThemeColours::componentBackground).brighter (0.1f));
        g.fillRect (b.toFloat());
    }
    else
    {
        g.setColour (parent->findColour (ThemeColours::componentBackground).darker (0.1f));
        g.fillRect (b.toFloat());
    }

    // Row background
    if (rowIsSelected)
    {
        g.setColour (sc.withAlpha (0.15f));
        g.fillRoundedRectangle (b.toFloat().reduced (2, 2), 5.0f);
        g.setColour (parent->findColour (ThemeColours::defaultText).withAlpha (0.75f));
        g.drawRoundedRectangle (b.toFloat().reduced (2, 2), 5.0f, 1.0f);
    }

    // Status dot
    g.setColour (sc);
    g.fillEllipse (9.0f, float (b.getCentreY()) - 5.0f, 10.0f, 10.0f);

    // Name
    String name = m.streamName.isEmpty() ? "Probe" : m.streamName;
    Colour textCol = parent->findColour (ThemeColours::defaultText);
    g.setColour (textCol);
    g.setFont (interSemiBold (14.0f));
    g.drawText (name, 26, b.getY(), width - 80, height / 2 - 3, Justification::bottomLeft);

    // Channel count
    g.setColour (textCol.withAlpha (0.75f));
    g.setFont (interRegular (12.0f));
    g.drawText (String (m.numChannels) + " ch", 26, b.getY() + height / 2 + 3, width - 80, height / 2 - 3, Justification::topLeft);

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
    viewport->setScrollBarThickness (12);
    addAndMakeVisible (viewport.get());

    // Header controls
    durationCombo = std::make_unique<ComboBox>();
    durationCombo->addItem ("10 s", 1);
    durationCombo->addItem ("30 s", 2);
    durationCombo->addItem ("60 s", 3);
    durationCombo->setSelectedId (2);
    durationCombo->onChange = [this]
    {
        const int id  = durationCombo->getSelectedId();
        const int sec = (id == 1) ? 10 : (id == 3) ? 60 : 30;
        processor->setDurationSeconds (sec);
    };
    processor->setDurationSeconds (30); // match default selection
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
    statusIndicator->setFont (interSemiBold (14.0f));
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
    for (int i = 0; i < localMetrics.size(); ++i)
        probeListBox->repaintRow (i);

    if (selectedProbe >= localMetrics.size())
        return;

    const auto& m = localMetrics[selectedProbe];
    content->rmsPanel->updateData (m);
    content->specPanel->updateData (m);
    content->snapPanel->updateData (m);
    content->spikePanel->updateData (m);

    // Update status indicator
    if (m.processingDone)
    {
        statusIndicator->setText ("DONE", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colour (0xff42a5f5));
    }
    else
    {
        statusIndicator->setText ("RUNNING", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colour (0xff4caf50));
    }
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
    g.setColour (findColour (ThemeColours::componentParentBackground));
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
