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

#include <array>
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
        case ProbeStatus::FAIL: return Colour (0xfff44336);
        default:                return Colours::grey;
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
    g.setFont (Font (FontOptions ("Inter", "Semi Bold", 11.0f)));
    g.drawText (text, r, Justification::centred, false);
}

static void drawStatusIndicator (Graphics& g, Rectangle<float> bounds, ProbeStatus status)
{
    g.setColour (QCColours::statusCol (status));
    g.fillEllipse (bounds);
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawEllipse (bounds, 1.0f);
}

// Font helpers — typefaces sized dynamically by each caller
static Font interRegular  (float size) { return Font (FontOptions ("Inter", "Regular",   size)); }
static Font interSemiBold (float size) { return Font (FontOptions ("Inter", "Semi Bold", size)); }
static Font firaCodeRegular (float size) { return Font (FontOptions ("Fira Code", "Regular", size)); }

// Maps a pixel y coordinate to a channel index, based on the current view range and total height. 
// Channels are indexed in display order (highest at the top), not original order.
static int channelForPixelRow (int y, int height, int viewChStart, int viewChEnd)
{
    const int viewCh = viewChEnd - viewChStart;
    if (viewCh <= 0 || height <= 0)
        return viewChStart;

    const int rowFromTop = y * viewCh / height;
    return jlimit (viewChStart, viewChEnd - 1, viewChEnd - 1 - rowFromTop);
}

// Draws Y-axis channel ticks for the visible channel range [viewChStart, viewChEnd).
// Automatically picks evenly-spaced channel indices within the view range.
// channelOrder — maps display row → original channel number (from ProbeMetrics::channelOrder)
// tickCol  — caller-supplied colour (use defaultText.withAlpha for secondary elements)
// fontSize — caller-computed dynamic size
static void drawChannelYTicks (Graphics& g, Rectangle<int> pb,
                                int viewChStart, int viewChEnd,
                                const std::vector<int>& channelOrder,
                                Colour tickCol, float fontSize)
{
    const int viewCh = viewChEnd - viewChStart;
    if (viewCh <= 0)
        return;

    // Helper: pixel y for the centre of channel t's row
    auto chCentreY = [&] (int t) -> float
    {
           const int rowFromTop = (viewChEnd - 1) - t;
           return float (pb.getY())
               + (float (rowFromTop) + 0.5f) / float (viewCh) * float (pb.getHeight());
    };

    // Always show first and last; fit as many equally-spaced intermediate ticks
    // as the height allows (minimum 40 px apart in pixel space).
    const int first = viewChStart;
    const int last  = viewChEnd - 1;

    // How many intervals (gaps) can we fit?
    const int maxIntervals = std::max (1, pb.getHeight() / 40);
    // Round interval to a whole number of channels
    const int interval = std::max (1, viewCh / maxIntervals);

    std::vector<int> ticks;
    ticks.push_back (first);
    for (int t = first + interval; t < last; t += interval)
        ticks.push_back (t);
    if (last > first)
    {
        // Drop the penultimate tick if it's too close to last
        if (ticks.size() > 1 && last - ticks.back() < (interval/2))
            ticks.pop_back();
        ticks.push_back (last);
    }

    g.setColour (tickCol);
    g.setFont (firaCodeRegular (fontSize));

    for (int t : ticks)
    {
        const float y   = chCentreY (t);
        const int textY = jlimit (pb.getY(), pb.getBottom() - 10, int (y) - 5);
        const int label = (t >= 0 && t < (int) channelOrder.size()) ? channelOrder[t] : t;
        g.drawText (String (label), pb.getX() - AXIS_L, textY, AXIS_L - 6, 10, Justification::centredRight);
        g.drawHorizontalLine (int (y), float (pb.getX()) - 4.0f, float (pb.getX()));
    }
}

// ─── ZoomablePanel ─────────────────────────────────────────────────────────────

static constexpr int MIN_ZOOM_CH = 8;

ZoomablePanel::ZoomablePanel()
{
    // SVG path data for the zoom-reset icon (zoom-cancel / zoom-reset from tabler-icons)
    static const String kResetZoomPaths =
        "M21 21l-6 -6 M3.268 12.043a7.017 7.017 0 0 0 6.634 4.957a7.012 7.012 0 0 0 7.043 -6.131a7 7 0 0 0 -5.314 -7.672a7.021 7.021 0 0 0 -8.241 4.403 M3 4v4h4";
    
    Path iconPath;
    iconPath.addPath (Drawable::parseSVGPath (kResetZoomPaths));

    resetZoomBtn = std::make_unique<ShapeButton> ("resetZoom", Colours::transparentBlack, Colours::transparentBlack, Colours::transparentBlack);
    resetZoomBtn->setShape (iconPath, true, true, false);
    resetZoomBtn->setTooltip ("Reset zoom");
    resetZoomBtn->setMouseCursor (MouseCursor::PointingHandCursor);
    resetZoomBtn->onClick = [this] { resetZoom(); };
    resetZoomBtn->setAlwaysOnTop (true);
    resetZoomBtn->setOutline (findColour (ThemeColours::defaultText), 1.5f);
    addAndMakeVisible (resetZoomBtn.get());
    resetZoomBtn->setVisible (false);
}

void ZoomablePanel::resized()
{
    resetZoomBtn->setBounds (getWidth() - 26, 10, 16, 16);
}

void ZoomablePanel::updateResetButtonVisibility()
{
    resetZoomBtn->setVisible (numCh > 0 && (viewChStart != 0 || viewChEnd != numCh));
}

void ZoomablePanel::initViewRange (int channelCount)
{
    numCh = channelCount;
    if (viewChEnd == 0 || viewChEnd > numCh)
    {
        viewChStart = 0;
        viewChEnd   = numCh;
    }
}

void ZoomablePanel::setViewRange (int start, int end)
{
    viewChStart = jlimit (0, std::max (0, numCh - MIN_ZOOM_CH), start);
    viewChEnd   = jlimit (MIN_ZOOM_CH, numCh, end);
    if (viewChEnd - viewChStart < MIN_ZOOM_CH)
        viewChEnd = std::min (numCh, viewChStart + MIN_ZOOM_CH);
    updateResetButtonVisibility();
    repaint();
}

void ZoomablePanel::resetZoom()
{
    setViewRange (0, numCh);
}

void ZoomablePanel::mouseDoubleClick (const MouseEvent& e)
{
    if (lastPb.contains (e.getPosition()))
        setViewRange (0, numCh);
}

void ZoomablePanel::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (numCh == 0 || !lastPb.contains (e.getPosition()))
    {
        Component::mouseWheelMove (e, w);
        return;
    }

    if (e.mods.isCommandDown())
    {
        // Cmd + wheel → zoom around cursor
        const int   viewCh   = viewChEnd - viewChStart;
        const float fracY    = jlimit (0.0f, 1.0f,
                               float (e.y - lastPb.getY()) / float (lastPb.getHeight()));
        const float cursorCh = float (viewChStart) + fracY * float (viewCh);
        const float factor   = w.deltaY < 0.0f ? 1.3f : 0.7f;
        const int   newCount = jlimit (MIN_ZOOM_CH, numCh, int (float (viewCh) * factor));
        int newStart = int (cursorCh - fracY * float (newCount));
        int newEnd   = newStart + newCount;
        if (newStart < 0)     { newStart = 0;    newEnd = newCount; }
        if (newEnd   > numCh) { newEnd   = numCh; newStart = numCh - newCount; }
        setViewRange (newStart, newEnd);
        return;
    }

    if (e.mods.isAltDown())
    {
        // Alt + wheel → pan the channel view
        const int viewCh = viewChEnd - viewChStart;
        if (lastPb.getHeight() <= 0)
            return;
        // Scroll ~10 % of the visible range per wheel tick, direction matches scroll
        const int step     = std::max (1, int (float (viewCh) * 0.10f));
        const int delta    = w.deltaY > 0.0f ? -step : step;
        const int newStart = jlimit (0, numCh - viewCh, viewChStart + delta);
        setViewRange (newStart, newStart + viewCh);
        return;
    }

    // No modifier — let the event propagate to the Viewport
    Component::mouseWheelMove (e, w);
}

void ZoomablePanel::colourChanged()
{
    resetZoomBtn->setOutline (findColour (ThemeColours::defaultText), 1.5f);
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
    threshUV            = m.rmsThresholdUV;
    numHighRms          = m.numHighRmsChannels;
    processingDone      = m.processingDone;

    // maxRms from all accumulated frames (not just latest)
    maxRms = 1.0f;
    const int validSamples = rmsHistoryFrames * m.numChannels;
    for (int i = 0; i < validSamples && i < (int) rmsHistory.size(); ++i)
        maxRms = std::max (maxRms, rmsHistory[i]);

    channelOrder = m.channelOrder;
    initViewRange (m.numChannels);
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
    // Labels above and below the bar
    g.setFont (firaCodeRegular (11.0f));
    g.setColour (findColour (ThemeColours::defaultText).withAlpha (0.85f));
    g.drawText (String (100) + "μV",
                int (r.getCentreX()) - 20, int (r.getY()) - 14,
                40, 12, Justification::centred);
    g.drawText (String (0) + "μV",
                int (r.getCentreX()) - 20, int (r.getBottom()) + 3,
                40, 12, Justification::centred);
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

    // Meta row: threshold label
    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    auto threshLabel = metaRow.removeFromLeft (160);
    g.drawText ("THRESHOLD  " + String (threshUV, 0) + " μV", threshLabel, Justification::centredLeft);

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

    // Meta row: alert badge
    if (numHighRms > 0)
        drawBadge (g, metaRow.reduced (2, 2).withRight( pb.getRight()), Colour (0xffc62828),
                   String (numHighRms) + " ch above " + String (threshUV, 0) + " μV threshold");

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    lastPb = pb;
    const int viewCh_rms = viewChEnd - viewChStart;
    if (viewCh_rms <= 0)
        return;

    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // --- Heatmap via BitmapData ---
    {
        Image heatmap (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        heatmap.clear (heatmap.getBounds(), findColour (ThemeColours::componentParentBackground));
        Image::BitmapData bmd (heatmap, Image::BitmapData::writeOnly);

        // Only paint frames that have been accumulated
        if (rmsHistoryFrames > 0)
        {
            const int filledPx = std::min (pw_i,
                int (int64_t (rmsHistoryFrames) * int64_t (pw_i) / int64_t (rmsHistoryMaxFrames)));

            for (int y = 0; y < ph_i; ++y)
            {
                const int ch = channelForPixelRow (y, ph_i, viewChStart, viewChEnd);
                for (int x = 0; x < filledPx; ++x)
                {
                    const int frame = std::min (rmsHistoryFrames - 1, std::max (0,
                        int (int64_t (x) * int64_t (rmsHistoryMaxFrames) / int64_t (pw_i))));
                    const float rms = rmsHistory[frame * numCh + ch];
                    const float t   = jlimit (0.0f, 1.0f, rms / 100.0f);
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
        const Colour bg = findColour (ThemeColours::componentParentBackground);
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), bg);
        if (! rmsUV.empty())
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = channelForPixelRow (y, sh, viewChStart, viewChEnd);
                const float v    = jlimit (1e-6f, 100.0f, rmsUV[ch]);
                const float t    = jlimit (0.0f, 1.0f, std::log10 (v) / 2.0f);
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
    g.setFont (firaCodeRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        const float frac = float (i) / 4.0f;
        const float x    = float (pb.getX()) + frac * float (pw_i);
        const int   sec  = int (frac * float (durationSec));
        g.drawVerticalLine (int (x) - std::floor (frac), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (sec), int (x) - 14, pb.getBottom() + 4, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (s)", pb.getX(), pb.getBottom() + 20, pw_i, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, viewChStart, viewChEnd, channelOrder, tickCol, tickSz);

    drawColourBar (g, cbarBounds);
}

// ─── PowerSpectrumPanel ───────────────────────────────────────────────────────

void PowerSpectrumPanel::updateData (const ProbeMetrics& m)
{
    spectrum           = m.powerSpectrum;
    sampleRate         = m.sampleRate;
    powerlineHz        = m.powerlineHz;
    numNoisyCh         = m.numNoisyChannels;
    channelPowerlineDb = m.channelPowerlineDb;
    channelHFNoiseDb   = m.channelHFNoiseDb;
    channelOrder       = m.channelOrder;
    initViewRange (m.numChannels);

    // Fixed dB range 0–100 for cross-probe comparability
    gMinDb  = 0.0f;
    gMaxDb  = 100.0f;
    hasData = ! spectrum.empty();

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
    // Labels above and below the bar
    g.setFont (firaCodeRegular (11.0f));
    g.setColour (findColour (ThemeColours::defaultText).withAlpha (0.85f));
    g.drawText (String (int (gMaxDb)) + "dB",
                int (r.getCentreX()) - 20, int (r.getY()) - 14,
                40, 12, Justification::centred);
    g.drawText (String (int (gMinDb)) + "dB",
                int (r.getCentreX()) - 20, int (r.getBottom()) + 3,
                40, 12, Justification::centred);
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

    b.reduce (0, PLOT_PAD);
    if (spectrum.empty())
        return;
    
        b.removeFromBottom (AXIS_B);

    // Colour bar on right
    auto cbarBounds = b.removeFromRight (CBAR_W + 4).toFloat();
    b.removeFromRight (PLOT_PAD);
    // Two per-channel overview strips: HF noise (rightmost) + powerline noise (left of it)
    auto stripZone    = b.removeFromRight (STRIP_W * 2 + 4);
    auto hfStripBounds = stripZone.removeFromRight (STRIP_W);
    stripZone.removeFromRight (4);          // gap between the two strips
    auto plStripBounds = stripZone;
    b.removeFromLeft (AXIS_L);
    b.removeFromRight (PLOT_PAD);
    auto pb = b;

    // Meta row: alert badge
    if (numNoisyCh > 0)
        drawBadge (g, metaRow.reduced (2, 2).withRight( pb.getRight()), Colour (0xffe65100), String (numNoisyCh) + " ch noisy");

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    lastPb = pb;
    const int viewCh_sp = viewChEnd - viewChStart;
    if (viewCh_sp <= 0)
        return;

    const float nyquist    = sampleRate / 2.0f;
    const float dbRange    = gMaxDb - gMinDb;
    const float hzPerBin   = sampleRate / float (FFT_SIZE);
    // Log-frequency axis: display from LOG_FREQ_MIN Hz up to Nyquist
    static constexpr float LOG_FREQ_MIN = 10.0f;
    const float logFMin = std::log10 (LOG_FREQ_MIN);
    const float logFMax = std::log10 (nyquist);
    // Helper: pixel x [0, pw_i) -> FFT bin index (log scale)
    auto pixelToLogBin = [&] (int x) -> int {
        const float fhz = std::pow (10.0f, logFMin + float (x) / float (pw_i) * (logFMax - logFMin));
        return jlimit (1, FFT_BINS - 1, int (fhz / hzPerBin));
    };
    // Helper: frequency Hz -> pixel x offset relative to pb.getX() (log scale)
    auto freqToLogPixelX = [&] (float hz) -> float {
        if (hz <= 0.0f) return -1.0f;
        return (std::log10 (hz) - logFMin) / (logFMax - logFMin) * float (pw_i);
    };

    // Draw border around plot area
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // Heatmap: render into an Image (one pixel per display pixel), then blit.
    // Each pixel's channel is determined by its Y coordinate; frequency bin by X.
    {
        Image heatmap (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        heatmap.clear (heatmap.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (hasData)
        {
            Image::BitmapData bmd (heatmap, Image::BitmapData::writeOnly);

            for (int y = 0; y < ph_i; ++y)
            {
                const int c = channelForPixelRow (y, ph_i, viewChStart, viewChEnd);
                const float* row = spectrum.data() + c * FFT_BINS;
                for (int x = 0; x < pw_i; ++x)
                {
                    const int   k    = pixelToLogBin (x);
                    const float db   = row[k] > 0.0f ? 10.0f * std::log10 (row[k]) : gMinDb;
                    const float tLin = jlimit (0.0f, 1.0f, (db - gMinDb) / dbRange);
                    const float t    = std::log10 (1.0f + 9.0f * tLin); // log colour scale
                    bmd.setPixelColour (x, y, ColourMaps::viridis (t));
                }
            }
        }

        g.drawImageAt (heatmap, pb.getX(), pb.getY());
    }

    // --- Powerline noise overview strip ---
    {
        const int sw = plStripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (! channelPowerlineDb.empty())
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = channelForPixelRow (y, sh, viewChStart, viewChEnd);
                const float v    = jlimit (1e-6f, 100.0f, std::max (0.0f, channelPowerlineDb[ch]));
                const float t    = jlimit (0.0f, 1.0f, std::log10 (v) / 2.0f);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = ColourMaps::viridis (t);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, plStripBounds.getX(), plStripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (plStripBounds.expanded (1), 1);
    }

    // --- HF noise overview strip (10–15 kHz) ---
    {
        const int sw = hfStripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (! channelHFNoiseDb.empty())
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = channelForPixelRow (y, sh, viewChStart, viewChEnd);
                const float v    = jlimit (1e-6f, 100.0f, std::max (0.0f, channelHFNoiseDb[ch]));
                const float t    = jlimit (0.0f, 1.0f, std::log10 (v) / 2.0f);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = ColourMaps::viridis (t);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, hfStripBounds.getX(), hfStripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (hfStripBounds.expanded (1), 1);
    }

    // Strip labels below each overview column
    g.setColour (tickCol);
    g.setFont (firaCodeRegular (tickSz));
    g.drawText (String (int (powerlineHz)) + " Hz",  plStripBounds.getX(), pb.getBottom() + 4, plStripBounds.getWidth(), 11, Justification::centred);
    g.drawText ("8-15 kHz", hfStripBounds.getX(), pb.getBottom() + 4, hfStripBounds.getWidth(), 11, Justification::centred);

    // Powerline harmonic marker
    const float harmonics[] = { powerlineHz };
    for (float h : harmonics)
    {
        if (h <= 0.0f || h > nyquist || h < LOG_FREQ_MIN)
            continue;
        const float px = freqToLogPixelX (h);
        if (px < 0.0f || px >= float (pw_i))
            continue;
        g.setColour (Colours::red.withAlpha (0.6f));
        g.drawVerticalLine (int (float (pb.getX()) + px), float (pb.getY()), float (pb.getBottom()));
    }

    // X axis: log-spaced frequency ticks
    g.setColour (tickCol);
    g.setFont (firaCodeRegular (tickSz));
    int lastLabelX = -100;
    for (float ft : { 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        if (ft < LOG_FREQ_MIN || ft > nyquist) continue;
        const float px = freqToLogPixelX (ft);
        if (px < 0.0f || px >= float (pw_i)) continue;
        const int x = int (float (pb.getX()) + px);
        g.drawVerticalLine (x, float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        if (x - lastLabelX >= 28)
        {
            String lbl = ft >= 1000.0f ? (String (int (ft / 1000.0f)) + "k") : String (int (ft));
            g.drawText (lbl, x - 14, pb.getBottom() + 4, 28, 12, Justification::centred);
            lastLabelX = x;
        }
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Frequency (Hz)", pb.getX(), pb.getBottom() + 20, pw_i, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, viewChStart, viewChEnd, channelOrder, tickCol, tickSz);

    drawColourBar (g, cbarBounds);
}

// ─── DataSnapshotPanel ────────────────────────────────────────────────────────
//   Y axis = channels (row per channel), X axis = time samples (columns)

void DataSnapshotPanel::updateData (const ProbeMetrics& m)
{
    snapshot = m.dataSnapshot;
    snapshotSamples = m.snapshotSamples;
    numSaturatedCh = m.numSaturatedChannels;
    saturationThresholdUV = m.snapshotSaturationThresholdUV;
    channelOrder = m.channelOrder;
    initViewRange (m.numChannels);

    // Compute standard deviation per channel for the live strip
    channelStdUV.assign (numCh, 0.0f);
    hasData = false;
    for (int c = 0; c < numCh && ! snapshot.empty(); ++c)
    {
        const float* row = snapshot.data() + c * snapshotSamples;
        float sum = 0.0f;
        for (int s = 0; s < snapshotSamples; ++s)
            sum += row[s];

        const float mean = sum / float (snapshotSamples);
        float sumSq = 0.0f;
        for (int s = 0; s < snapshotSamples; ++s)
        {
            const float d = row[s] - mean;
            sumSq += d * d;
            if (row[s] != 0.0f) hasData = true;
        }
        channelStdUV[c] = std::sqrt (sumSq / float (snapshotSamples));
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

    auto metaRow = b.removeFromTop (META_H);
    g.setColour (tickCol);
    g.setFont (interRegular (metaSz));
    auto windowLabel = metaRow.removeFromLeft (260);
    g.drawText ("WINDOW SIZE " + String (SNAPSHOT_WINDOW_MS) + " ms", windowLabel, Justification::centredLeft);

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

    if (numSaturatedCh > 0)
        drawBadge (g, metaRow.reduced (2, 2).withRight (pb.getRight()), Colour (0xffe65100),
                   String (numSaturatedCh) + " ch saturated above " + String (saturationThresholdUV, 0) + " μV");

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    lastPb = pb;
    const int viewCh_sn = viewChEnd - viewChStart;
    if (viewCh_sn <= 0)
        return;

    // Draw border around plot area
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    constexpr float minUV = -100.0f;
    constexpr float maxUV =  100.0f;
    const float range = maxUV - minUV;  // 200.0f

    // --- Snapshot via BitmapData ---
    {
        Image snapshotImg (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        snapshotImg.clear (snapshotImg.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (hasData)
        {
            Image::BitmapData bmd (snapshotImg, Image::BitmapData::writeOnly);

            // Average-pool over samples per pixel column (reduces aliasing).
            for (int y = 0; y < ph_i; ++y)
            {
                const int c = channelForPixelRow (y, ph_i, viewChStart, viewChEnd);
                const float* row = snapshot.data() + c * snapshotSamples;
                for (int x = 0; x < pw_i; ++x)
                {
                    const int sStart = x       * snapshotSamples / pw_i;
                    const int sEnd   = std::max (sStart + 1, (x + 1) * snapshotSamples / pw_i);
                    float sum = 0.0f;

                    for (int s = sStart; s < sEnd && s < snapshotSamples; ++s)
                        sum += row[s];

                    const float val = sum / float (sEnd - sStart);
                    const float t = jlimit (0.0f, 1.0f, (val - minUV) / range);
                    bmd.setPixelColour (x, y, ColourMaps::cividis (t));
                }
            }
        }

        g.drawImageAt (snapshotImg, pb.getX(), pb.getY());
    }

    // --- Live per-channel strip (mean |voltage|, Cividis, 0 → 100 µV linear) ---
    {
        const int sw = stripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (! channelStdUV.empty())
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = channelForPixelRow (y, sh, viewChStart, viewChEnd);
                const float t    = jlimit (0.0f, 1.0f, channelStdUV[ch] / 100.0f);
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

    // Colour bar (labels above and below)
    for (int y = 0; y < int (cbar.getHeight()); ++y)
    {
        g.setColour (ColourMaps::cividis (1.0f - float (y) / cbar.getHeight()));
        g.fillRect (cbar.getX(), cbar.getY() + float (y), cbar.getWidth(), 1.0f);
    }
    g.setFont (firaCodeRegular (11.0f));
    g.setColour (findColour (ThemeColours::defaultText).withAlpha (0.85f));
    g.drawText (String("+100") + "μV",
                int (cbar.getCentreX()) - 20, int (cbar.getY()) - 14,
                40, 12, Justification::centred);
    g.drawText (String("-100") + "μV",
                int (cbar.getCentreX()) - 20, int (cbar.getBottom()) + 3,
                40, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, viewChStart, viewChEnd, channelOrder, tickCol, tickSz);

    // X axis: time sample ticks
    g.setColour (tickCol);
    g.setFont (firaCodeRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        float frac = float (i) / 4.0f;
        float x    = float (pb.getX()) + frac * float (pb.getWidth());
        int   samp = int (frac * float (snapshotSamples));
        g.drawVerticalLine (int (x), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (samp), int (x) - 14, pb.getBottom() + 4, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (samples)", pb.getX(), pb.getBottom() + 20, pb.getWidth(), 12, Justification::centred);
}

// ─── SpikeRatePanel ───────────────────────────────────────────────────────────
//   Y axis = channels, X axis = time (one column per spike-rate window ~200 ms)

void SpikeRatePanel::updateData (const ProbeMetrics& m)
{
    rateHz               = m.spikeRateHz;
    spikeRateHistory     = m.spikeRateHistory;
    historyFrames        = m.spikeRateHistoryFrames;
    historyMaxFrames     = std::max (1, m.rmsHistoryMaxFrames);
    durationSec          = std::max (1, m.analysisDurationSec);
    processingDone       = m.processingDone;
    spikeFailHz          = m.spikeRateFailHz;
    spikeLowHz           = m.spikeRateLowHz;
    numLowCh             = m.numLowSpikeChannels;
    channelOrder         = m.channelOrder;
    initViewRange (m.numChannels);
    repaint();
}

void SpikeRatePanel::drawColourBar (Graphics& g, Rectangle<float> r)
{
    for (int y = 0; y < int (r.getHeight()); ++y)
    {
        float t = 1.0f - float (y) / r.getHeight();
        g.setColour (ColourMaps::turbo (t));
        g.fillRect (r.getX(), r.getY() + float (y), r.getWidth(), 1.0f);
    }
    g.setFont (firaCodeRegular (11.0f));
    g.setColour (findColour (ThemeColours::defaultText).withAlpha (0.85f));
    g.drawText ("100Hz",
                int (r.getCentreX()) - 20, int (r.getY()) - 14,
                40, 12, Justification::centred);
    g.drawText ("0Hz",
                int (r.getCentreX()) - 20, int (r.getBottom()) + 3,
                40, 12, Justification::centred);
}

void SpikeRatePanel::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (findColour (ThemeColours::componentBackground));

    // Border
    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (b, 1);

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

    b.reduce (0, PLOT_PAD);
    if (spikeRateHistory.empty())
        return;

    b.removeFromBottom (AXIS_B);

    // Colour bar on right
    auto cbarBounds = b.removeFromRight (CBAR_W + 4).toFloat();
    b.removeFromRight (PLOT_PAD);
    // Average spike rate overview strip
    auto stripBounds = b.removeFromRight (STRIP_W);
    b.removeFromRight (PLOT_PAD);
    b.removeFromLeft (AXIS_L);
    auto pb = b;

    if (numLowCh > 0)
        drawBadge (g, metaRow.reduced (2, 2).withRight( pb.getRight()), Colour (0xffc62828),
                   String (numLowCh) + " ch below " + String (spikeFailHz, 1) + " Hz threshold");

    const int pw_i = pb.getWidth();
    const int ph_i = pb.getHeight();
    if (pw_i <= 0 || ph_i <= 0)
        return;

    lastPb = pb;
    const int viewCh_sr = viewChEnd - viewChStart;
    if (viewCh_sr <= 0)
        return;

    g.setColour (findColour (ThemeColours::outline));
    g.drawRect (pb.expanded (1), 1);

    // --- Heatmap: live spike rate history (channels × time) ---
    {
        Image heatmap (Image::RGB, pw_i, ph_i, true, SoftwareImageType());
        heatmap.clear (heatmap.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (historyFrames > 0)
        {
            Image::BitmapData bmd (heatmap, Image::BitmapData::writeOnly);
            const int filledPx = std::min (pw_i,
                int (int64_t (historyFrames) * int64_t (pw_i) / int64_t (historyMaxFrames)));
            for (int y = 0; y < ph_i; ++y)
            {
                const int ch = channelForPixelRow (y, ph_i, viewChStart, viewChEnd);
                for (int x = 0; x < filledPx; ++x)
                {
                    const int   frame = std::min (historyFrames - 1, std::max (0,
                        int (int64_t (x) * int64_t (historyMaxFrames) / int64_t (pw_i))));
                    const float hz = spikeRateHistory[frame * numCh + ch];
                    const float t  = jlimit (0.0f, 1.0f, hz / 100.0f);
                    bmd.setPixelColour (x, y, ColourMaps::turbo (t));
                }
            }
        }
        g.drawImageAt (heatmap, pb.getX(), pb.getY());
    }

    // --- Average spike rate overview strip (log scale, threshold colours) ---
    {
        const int sw = stripBounds.getWidth();
        const int sh = ph_i;
        Image stripImg (Image::RGB, sw, sh, true, SoftwareImageType());
        stripImg.clear (stripImg.getBounds(), findColour (ThemeColours::componentParentBackground));
        if (! rateHz.empty())
        {
            Image::BitmapData bmd (stripImg, Image::BitmapData::writeOnly);
            for (int y = 0; y < sh; ++y)
            {
                const int   ch   = channelForPixelRow (y, sh, viewChStart, viewChEnd);
                const float rate = rateHz[ch];
                const float v    = jlimit (1e-6f, 100.0f, rate);
                const float t    = jlimit (0.0f, 1.0f, std::log10 (v) / 2.0f);
                const int   barW = jlimit (0, sw, int (t * float (sw)));
                const Colour col = rate < spikeFailHz ? Colour (0xfff44336)
                                 : rate < spikeLowHz  ? Colour (0xffff9800)
                                                      : Colour (0xff42a5f5);
                for (int x = 0; x < barW; ++x)
                    bmd.setPixelColour (x, y, col);
            }
        }
        g.drawImageAt (stripImg, stripBounds.getX(), stripBounds.getY());
        g.setColour (findColour (ThemeColours::outline));
        g.drawRect (stripBounds.expanded (1), 1);
    }

    // Progress line (white vertical bar at current frame)
    if (! processingDone && historyFrames > 0 && historyMaxFrames > 0)
    {
        const float progX = float (pb.getX())
                          + float (historyFrames) / float (historyMaxFrames)
                          * float (pw_i);
        g.setColour (Colours::white.withAlpha (0.6f));
        g.drawVerticalLine (int (progX), float (pb.getY()), float (pb.getBottom()));
    }

    // X axis: time ticks
    g.setColour (tickCol);
    g.setFont (firaCodeRegular (tickSz));
    for (int i = 0; i <= 4; ++i)
    {
        const float frac = float (i) / 4.0f;
        const float x    = float (pb.getX()) + frac * float (pw_i);
        const int   sec  = int (frac * float (durationSec));
        g.drawVerticalLine (int (x) - int (std::floor (frac)), float (pb.getBottom()), float (pb.getBottom()) + 4.0f);
        g.drawText (String (sec), int (x) - 14, pb.getBottom() + 4, 28, 12, Justification::centred);
    }
    g.setFont (interRegular (metaSz));
    g.drawText ("Time (s)", pb.getX(), pb.getBottom() + 20, pw_i, 12, Justification::centred);

    // Y axis: channel ticks
    drawChannelYTicks (g, pb, viewChStart, viewChEnd, channelOrder, tickCol, tickSz);

    drawColourBar (g, cbarBounds);
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

    // Name
    String name = m.streamName.isEmpty() ? "Probe" : m.streamName;
    Colour textCol = parent->findColour (ThemeColours::defaultText);
    g.setColour (textCol);
    g.setFont (interSemiBold (14.0f));
    g.drawText (name, 12, b.getY(), width - 80, height / 2 - 3, Justification::bottomLeft);

    // Channel count
    g.setColour (textCol.withAlpha (0.75f));
    g.setFont (interRegular (12.0f));
    g.drawText (String (m.numChannels) + " ch", 12, b.getY() + height / 2 + 3, width - 96, height / 2 - 3, Justification::topLeft);

    const std::array<ProbeStatus, 4> plotStatuses { m.rmsStatus, m.spectrumStatus, m.snapshotStatus, m.spikeStatus };
    Rectangle<float> indicatorArea = b.removeFromRight (66).toFloat().reduced (10.0f, 0.0f);
    const float dotSize = 9.0f;
    const float totalDotsWidth = dotSize * float (plotStatuses.size());
    const float gap = plotStatuses.size() > 1
        ? (indicatorArea.getWidth() - totalDotsWidth) / float (plotStatuses.size() - 1)
        : 0.0f;
    float dotX = indicatorArea.getX();
    const float dotY = indicatorArea.getCentreY() - dotSize * 0.5f;
    for (ProbeStatus plotStatus : plotStatuses)
    {
        drawStatusIndicator (g, Rectangle<float> (dotX, dotY, dotSize, dotSize), plotStatus);
        dotX += dotSize + gap;
    }
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

void ContentComponent::setLayout (PanelLayout l)
{
    currentLayout = l;
    resized();
}

void ContentComponent::resized()
{
    auto b = getLocalBounds();
    switch (currentLayout)
    {
        case PanelLayout::Grid2x2:
        {
            auto top = b.removeFromTop (b.getHeight() / 2);
            rmsPanel->setBounds  (top.removeFromLeft (top.getWidth() / 2));
            specPanel->setBounds (top);
            snapPanel->setBounds (b.removeFromLeft (b.getWidth() / 2));
            spikePanel->setBounds (b);
            break;
        }
        case PanelLayout::Stack4x1:
        {
            const int h = b.getHeight() / 4;
            rmsPanel->setBounds   (b.removeFromTop (h));
            specPanel->setBounds  (b.removeFromTop (h));
            snapPanel->setBounds  (b.removeFromTop (h));
            spikePanel->setBounds (b);
            break;
        }
        case PanelLayout::Stack1x4:
        {
            const int w = b.getWidth() / 4;
            rmsPanel->setBounds   (b.removeFromLeft (w));
            specPanel->setBounds  (b.removeFromLeft (w));
            snapPanel->setBounds  (b.removeFromLeft (w));
            spikePanel->setBounds (b);
            break;
        }
    }
}

// ─── LayoutButton ────────────────────────────────────────────────────────────

class LayoutButton : public Button
{
public:
    enum class Style { Fill, Stroke };

    LayoutButton (const String& name, Path p)
        : Button (name), path (std::move (p))
    {
        setClickingTogglesState (false);
    }

    void paintButton (Graphics& g, bool isHighlighted, bool /*isDown*/) override
    {
        const bool on = getToggleState();
        const Colour c = isHighlighted ? findColour (ThemeColours::defaultText).withAlpha (0.65f)
                                       : findColour (ThemeColours::defaultText);
        if (on)
            g.setColour (findColour (ThemeColours::highlightedFill));
        else
            g.setColour (findColour (ThemeColours::widgetBackground));

        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

        const auto iconBounds = getLocalBounds().reduced (3).toFloat();
        const auto t = path.getTransformToScaleToFit (iconBounds, true);
        g.setColour (c);
        g.drawRoundedRectangle (iconBounds, 3.0f, 1.0f);
        g.strokePath (path, PathStrokeType (1.0f), t);
    }
    
private:
    Path  path;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayoutButton)
};

// SVG path d-strings for each layout icon
// Grid
static const char* PATH_D_GRID = "M3 12h18m-9-9v18";

// HStack
static const char* PATH_D_HSTACK = "M7.5 3v18M12 3v18m4.5-18v18";

// VStack
static const char* PATH_D_VSTACK = "M21 7.5H3M21 12H3m18 4.5H3";

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

    durationLabel = std::make_unique<Label>();
    durationLabel->setText ("Duration", dontSendNotification);
    durationLabel->setFont (interRegular (16.0f));
    durationLabel->attachToComponent (durationCombo.get(), true);
    addAndMakeVisible (durationLabel.get());

    autoStartBtn = std::make_unique<ToggleButton> ("Auto Start");
    autoStartBtn->setClickingTogglesState (true);
    autoStartBtn->setToggleState (true, dontSendNotification); // default ON
    autoStartBtn->setTooltip ("Begin processing as soon as acquisition starts");
    autoStartBtn->onClick = [this]
    {
        processor->setAutoStart (autoStartBtn->getToggleState());
    };
    addAndMakeVisible (autoStartBtn.get());

    captureBtn = std::make_unique<TextButton> ("Capture");
    captureBtn->setColour (TextButton::buttonColourId, Colour (0xff1976d2));
    captureBtn->setTooltip ("Manually start data processing");
    captureBtn->setEnabled (false); // only enable when acquisition is active
    captureBtn->onClick = [this]
    {
        if (processor->isProcessingActive())
            stopProcessing();
        else
            startProcessing();
    };
    addAndMakeVisible (captureBtn.get());

    saveBtn = std::make_unique<TextButton> ("Save");
    saveBtn->setColour (TextButton::buttonColourId, Colour (0xff388e3c));
    addAndMakeVisible (saveBtn.get());

    // Layout toggle buttons
    auto makeLayoutBtn = [&] (std::unique_ptr<Button>& btn,
                              const String& name, const char* pathD)
    {
        auto lb = std::make_unique<LayoutButton> (
            name, Drawable::parseSVGPath (pathD));
        lb->setTooltip (name);
        btn = std::move (lb);
        addAndMakeVisible (btn.get());
    };

    makeLayoutBtn (layoutGridBtn,   "Grid 2x2",       PATH_D_GRID);
    makeLayoutBtn (layoutHStackBtn, "Horizontal 4x1", PATH_D_HSTACK);
    makeLayoutBtn (layoutVStackBtn, "Vertical 1x4",   PATH_D_VSTACK);

    // Default: grid selected
    layoutGridBtn->setToggleState (true, dontSendNotification);

    layoutGridBtn->onClick = [this]
    {
        content->setLayout (ContentComponent::PanelLayout::Grid2x2);
        layoutPanels();
        layoutGridBtn->setToggleState   (true,  dontSendNotification);
        layoutHStackBtn->setToggleState (false, dontSendNotification);
        layoutVStackBtn->setToggleState (false, dontSendNotification);
    };
    layoutHStackBtn->onClick = [this]
    {
        content->setLayout (ContentComponent::PanelLayout::Stack4x1);
        layoutPanels();
        layoutGridBtn->setToggleState   (false, dontSendNotification);
        layoutHStackBtn->setToggleState (true,  dontSendNotification);
        layoutVStackBtn->setToggleState (false, dontSendNotification);
    };
    layoutVStackBtn->onClick = [this]
    {
        content->setLayout (ContentComponent::PanelLayout::Stack1x4);
        layoutPanels();
        layoutGridBtn->setToggleState   (false, dontSendNotification);
        layoutHStackBtn->setToggleState (false, dontSendNotification);
        layoutVStackBtn->setToggleState (true,  dontSendNotification);
    };

    statusIndicator = std::make_unique<Label>();
    statusIndicator->setJustificationType (Justification::centred);
    statusIndicator->setText ("IDLE", dontSendNotification);
    statusIndicator->setColour (Label::textColourId, Colours::orangered);
    statusIndicator->setFont (interSemiBold (16.0f));
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

    snapRefreshCounter = 0;

    content->rmsPanel->resetZoom();
    content->specPanel->resetZoom();
    content->snapPanel->resetZoom();
    content->spikePanel->resetZoom();

    layoutPanels();
    refresh();
}

void QualityMonitorCanvas::beginAnimation()
{
    startCallbacks();
    acquisitionActive = true;
    processingDone = false;

    if (autoStartBtn->getToggleState())
    {
        statusIndicator->setText ("RUNNING", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colours::royalblue);

        saveBtn->setEnabled (false); // only enable after processing is done

        captureBtn->setButtonText ("Stop");
        captureBtn->setColour (TextButton::buttonColourId, Colour (0xffd32f2f));
        captureBtn->setTooltip ("Stop the current analysis run");
        captureBtn->setEnabled (true);
        
        durationCombo->setEnabled (false);
    }
    else
    {
        captureBtn->setEnabled (true);
    }
}

void QualityMonitorCanvas::endAnimation()
{
    stopCallbacks();
    acquisitionActive = false;

    if (processingDone)
    {
        statusIndicator->setText ("DONE", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colours::seagreen);
        saveBtn->setEnabled (true); // enable after processing is done
    }
    else
    {
        statusIndicator->setText ("IDLE", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colours::orangered);
    }

    captureBtn->setButtonText ("Capture");
    captureBtn->setColour (TextButton::buttonColourId, Colour (0xff1976d2));
    captureBtn->setTooltip ("Manually start data processing");
    captureBtn->setEnabled (false);

    durationCombo->setEnabled (true);
}

void QualityMonitorCanvas::startProcessing()
{
    processor->startProcessing();

    processingDone = false;
    statusIndicator->setText ("RUNNING", dontSendNotification);
    statusIndicator->setColour (Label::textColourId, Colours::royalblue);
    saveBtn->setEnabled (false); // only enable after processing is done

    captureBtn->setButtonText ("Stop");
    captureBtn->setColour (TextButton::buttonColourId, Colour (0xffd32f2f));
    captureBtn->setTooltip ("Stop the current analysis run");
    captureBtn->setEnabled (true);

    durationCombo->setEnabled (false);
}

void QualityMonitorCanvas::stopProcessing()
{
    processor->stopProcessing();

    statusIndicator->setText ("IDLE", dontSendNotification);
    statusIndicator->setColour (Label::textColourId, Colours::orangered);

    captureBtn->setButtonText ("Capture");
    captureBtn->setColour (TextButton::buttonColourId, Colour (0xff1976d2));
    captureBtn->setTooltip ("Manually start data processing");
    captureBtn->setEnabled (true);

    durationCombo->setEnabled (true);
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

    // refresh snapshot panel at 1 Hz (canvas runs at 5 Hz)
    if (snapRefreshCounter == 0)
    {
        content->snapPanel->updateData (m);
    }
    snapRefreshCounter = (snapRefreshCounter + 1) % int (refreshRate);

    content->spikePanel->updateData (m);

    processingDone = m.processingDone;

    if (processingDone)
    {
        statusIndicator->setText ("DONE", dontSendNotification);
        statusIndicator->setColour (Label::textColourId, Colours::seagreen);
        saveBtn->setEnabled (true); // enable after processing is done

        captureBtn->setButtonText ("Capture");
        captureBtn->setColour (TextButton::buttonColourId, Colour (0xff1976d2));
        captureBtn->setTooltip ("Manually start data processing");
        captureBtn->setEnabled (true);

        durationCombo->setEnabled (true);
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
    auto hdr = b.removeFromTop (HEADER_H).reduced (4, 5);
    hdr.removeFromLeft (70); // leave space for duration label
    durationCombo->setBounds (hdr.removeFromLeft (80));
    hdr.removeFromLeft (10);
    autoStartBtn->setBounds (hdr.removeFromLeft (100).reduced (0, 2));
    hdr.removeFromLeft (10);
    captureBtn->setBounds (hdr.removeFromLeft (72));
    hdr.removeFromLeft (10);
    saveBtn->setBounds (hdr.removeFromLeft (60));
    hdr.removeFromLeft (10);

    hdr.removeFromRight (10);
    {
        const int btnSz = hdr.getHeight();
        layoutVStackBtn->setBounds   (hdr.removeFromRight (btnSz));
        hdr.removeFromRight (5);
        layoutHStackBtn->setBounds (hdr.removeFromRight (btnSz));
        hdr.removeFromRight (5);
        layoutGridBtn->setBounds (hdr.removeFromRight (btnSz));
    }
    hdr.removeFromRight (10);

    // Sidebar
    auto sb = b.removeFromLeft (SIDEBAR_W);
    // "PROBES" label occupies top 22 px; ListBox fills the rest
    sb.removeFromTop (30);
    sb.setHeight (probeListBox->getRowHeight() * probeListModel->getNumRows() + 2);
    probeListBox->setBounds (sb.reduced (1, 0));

    statusIndicator->setBounds (b.getCentreX() - 50, hdr.getY(), 100, hdr.getHeight() - 2);

    // Viewport fills remaining area; content is sized to at least the viewport
    viewport->setBounds (b);
    int cw, ch;
    switch (content->currentLayout)
    {
        case ContentComponent::PanelLayout::Stack1x4:
            cw = std::max (b.getWidth(),  4 * ContentComponent::MIN_PANEL_W);
            ch = std::max (b.getHeight(), ContentComponent::MIN_PANEL_H);
            break;
        case ContentComponent::PanelLayout::Stack4x1:
            cw = std::max (b.getWidth(),  ContentComponent::MIN_PANEL_W);
            ch = std::max (b.getHeight(), 4 * ContentComponent::MIN_PANEL_H);
            break;
        default: // Grid2x2
            cw = std::max (b.getWidth(),  2 * ContentComponent::MIN_PANEL_W);
            ch = std::max (b.getHeight(), 2 * ContentComponent::MIN_PANEL_H);
            break;
    }

    if (cw > b.getWidth())
        ch -= 12; // reduce height to accommodate horizontal scrollbar if needed

    if (ch > b.getHeight())
        cw -= 12; // reduce width to accommodate vertical scrollbar if needed

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

    if (statusIndicator != nullptr)
    {
        // Status indicator background
        g.setColour (findColour (ThemeColours::widgetBackground));
        g.fillRoundedRectangle (statusIndicator->getBounds().toFloat(), 3.0f);
    }

    // Sidebar background
    g.setColour (findColour (ThemeColours::componentParentBackground));
    g.fillRect (0, HEADER_H, SIDEBAR_W, getHeight() - HEADER_H);
    g.setColour (findColour (ThemeColours::outline));
    g.fillRect (SIDEBAR_W - 1, HEADER_H, 1, getHeight() - HEADER_H);

    // "Data Streams" label in sidebar
    g.setColour (findColour (ThemeColours::defaultFill));
    g.fillRect (0, HEADER_H, SIDEBAR_W - 1, 30);
    g.setColour (findColour (ThemeColours::defaultText));
    g.setFont (interSemiBold (14.0f));
    g.drawText ("DATA STREAMS", 10, HEADER_H, SIDEBAR_W - 12, 30, Justification::centredLeft);
}
