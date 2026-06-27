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

#ifndef QUALITYMONITORCANVAS_H_DEFINED
#define QUALITYMONITORCANVAS_H_DEFINED

#include "ColorMap.h"
#include "ProbeMetrics.h"
#include <AllLookAndFeels.h>
#include <UIUtilitiesHeaders.h>
#include <VisualizerWindowHeaders.h>
#include <functional>
#include <vector>

class QualityMonitor;
class QualityMonitorCanvas;

namespace QCColours
{
Colour statusCol (ProbeStatus s);
} // namespace QCColours

struct PanelLayoutCache
{
    Rectangle<int> titleBounds;
    Rectangle<int> badgeBounds;
    Rectangle<int> plotBounds;
    Rectangle<int> primaryStripBounds;
    Rectangle<int> secondaryStripBounds;
    Rectangle<int> colourBarBounds;
};

// ─── ZoomablePanel ────────────────────────────────────────────────────────────
// Abstract base for all four metric panels.
// Owns the shared Y-axis zoom/pan state and mouse interaction logic.
// Subclasses implement paint() and updateData(), and call initViewRange() once
// the channel count is known.
class ZoomablePanel : public Component
{
public:
    ZoomablePanel();

    // Called by subclass updateData() after numCh is set.
    void initViewRange (int channelCount);

    // Zoom to an explicit channel range [start, end).
    void setViewRange (int start, int end);

    // Restore full-probe view.
    void resetZoom();

    Rectangle<int> getPlotBoundsForSnapshot() const { return lastPb; }

    // Cmd+wheel = zoom, Alt+wheel = pan, unmodified wheel = pass to Viewport.
    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override;
    void mouseDoubleClick (const MouseEvent& e) override;

    // Updates the outline colour of the reset-zoom button when the theme changes.
    void colourChanged() override;

protected:
    int numCh = 0;
    int viewChStart = 0;
    int viewChEnd = 0;
    Rectangle<int> lastPb; // cached plot-area bounds from most recent paint()
    std::vector<int> channelOrder; // displayRow → original channel number (set by updateData)
    Rectangle<int> getHeaderControlBounds (int width, int columnIndex = 0, int totalColumns = 1, int rowHeight = 18, int columnGap = 6) const;
    void updateResetButtonBounds();

private:
    void updateResetButtonVisibility();

    std::unique_ptr<ShapeButton> resetZoomBtn;
};

// ─── RmsHeatmapPanel ─────────────────────────────────────────────────────────
// Y axis = channels, X axis = time (one column per RMS window ~1 s)
class RmsHeatmapPanel : public ZoomablePanel
{
public:
    RmsHeatmapPanel() = default;
    void bindThresholdParameters (Parameter* thresholdParameter, Parameter* channelPercentageParameter);
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;
    void resized() override;

private:
    std::vector<float> rmsUV;
    std::vector<float> rmsHistory;
    int rmsHistoryFrames = 0;
    int rmsHistoryMaxFrames = 150;
    int durationSec = 30;
    float threshUV = 20.0f;
    float failChannelPercentage = 50.0f;
    int numHighRms = 0;
    float maxRms = 1.0f;
    bool processingDone = false;
    PanelLayoutCache panelLayout;
    std::unique_ptr<BoundedValueParameterEditor> thresholdEditor;
    std::unique_ptr<BoundedValueParameterEditor> channelPercentageEditor;
    void drawColourBar (Graphics& g, Rectangle<float> r);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RmsHeatmapPanel)
};

// ─── PowerSpectrumPanel ──────────────────────────────────────────────────────
// Channels on Y axis, frequency on X axis — power rendered as heatmap
class PowerSpectrumPanel : public ZoomablePanel
{
public:
    PowerSpectrumPanel() = default;
    void bindNoiseThresholdParameters (Parameter* thresholdParameter, Parameter* channelPercentageParameter);
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;
    void resized() override;

private:
    std::vector<float> spectrum;
    std::vector<float> channelPowerlineDb; // per-channel powerline band power (dB), copied from ProbeMetrics
    std::vector<float> channelHFNoiseDb; // per-channel 8–15 kHz mean power (dB), copied from ProbeMetrics
    float sampleRate = 30000.0f;
    float powerlineHz = 60.0f;
    float powerlineSNRThresh = 10.0f;
    float failChannelPercentage = 50.0f;
    int numNoisyCh = 0;
    float gMinDb = -120.0f;
    float gMaxDb = 0.0f;
    bool hasData = false;
    PanelLayoutCache panelLayout;
    std::unique_ptr<BoundedValueParameterEditor> noiseThresholdEditor;
    std::unique_ptr<BoundedValueParameterEditor> channelPercentageEditor;
    void drawColourBar (Graphics& g, Rectangle<float> r);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerSpectrumPanel)
};

// ─── DataSnapshotPanel ───────────────────────────────────────────────────────
// Channels on Y axis, time samples on X axis (heatmap)
class DataSnapshotPanel : public ZoomablePanel
{
public:
    DataSnapshotPanel() = default;
    void bindThresholdParameters (Parameter* thresholdParameter, Parameter* channelPercentageParameter);
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;
    void resized() override;

private:
    std::vector<float> snapshot;
    std::vector<float> channelStdUV;
    int snapshotSamples = 3000;
    int numSaturatedCh = 0;
    float saturationThresholdUV = SNAPSHOT_SATURATION_THRESHOLD_UV;
    float failChannelPercentage = 50.0f;
    bool hasData = false;
    PanelLayoutCache panelLayout;
    std::unique_ptr<BoundedValueParameterEditor> thresholdEditor;
    std::unique_ptr<BoundedValueParameterEditor> channelPercentageEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DataSnapshotPanel)
};

// ─── SpikeRatePanel ──────────────────────────────────────────────────────────
// Channels on Y axis, spike rate on X axis (horizontal bar chart)
class SpikeRatePanel : public ZoomablePanel
{
public:
    SpikeRatePanel() = default;
    void bindThresholdParameters (Parameter* failParameter, Parameter* channelPercentageParameter);
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;
    void resized() override;

private:
    std::vector<float> rateHz; // average rate per channel (overview strip)
    std::vector<float> spikeRateHistory; // [historyMaxFrames * numChannels], Hz (heatmap)
    int historyFrames = 0;
    int historyMaxFrames = 150;
    int durationSec = 30;
    bool processingDone = false;
    float spikeFailHz = 0.1f;
    float failChannelPercentage = 50.0f;
    int numLowCh = 0;
    PanelLayoutCache panelLayout;
    std::unique_ptr<BoundedValueParameterEditor> failThresholdEditor;
    std::unique_ptr<BoundedValueParameterEditor> channelPercentageEditor;
    void drawColourBar (Graphics& g, Rectangle<float> r);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeRatePanel)
};

// ─── ProbeListModel ──────────────────────────────────────────────────────────
// JUCE ListBoxModel for the sidebar probe list
class ProbeListModel : public ListBoxModel
{
public:
    ProbeListModel (QualityMonitorCanvas* parent);

    void setMetrics (const Array<ProbeMetrics>& metrics, int selected);

    int getNumRows() override;
    void paintListBoxItem (int row, Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    std::function<void (int)> onProbeSelected;

private:
    Array<ProbeMetrics> localMetrics;
    int selectedIdx = 0;
    QualityMonitorCanvas* parent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProbeListModel)
};

// ─── ContentComponent ────────────────────────────────────────────────────────
// Inner scrollable content holding the four plot panels.
// Supports three layout modes: 2×2 grid, 4×1 vertical stack, 1×4 horizontal stack.
// Enforces minimum dimensions so plots stay readable at small window sizes.
class ContentComponent : public Component
{
public:
    enum class PanelLayout
    {
        Grid2x2,
        Stack4x1,
        Stack1x4
    };

    ContentComponent();

    std::unique_ptr<RmsHeatmapPanel> rmsPanel;
    std::unique_ptr<PowerSpectrumPanel> specPanel;
    std::unique_ptr<DataSnapshotPanel> snapPanel;
    std::unique_ptr<SpikeRatePanel> spikePanel;

    static constexpr int MIN_PANEL_W = 600;
    static constexpr int MIN_PANEL_H = 400;

    PanelLayout currentLayout = PanelLayout::Grid2x2;
    void setLayout (PanelLayout l);

    void resized() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentComponent)
};

// ─── QualityMonitorCanvas ────────────────────────────────────────────────────
/** Draws data in real time */
class QualityMonitorCanvas : public Visualizer
{
public:
    /** Constructor */
    QualityMonitorCanvas (QualityMonitor* processor);

    /** Destructor */
    ~QualityMonitorCanvas();

    /** Updates boundaries of sub-components whenever the canvas size changes */
    void resized() override;

    /** Called by the update() method to allow the visualizer to update its custom settings. */
    void updateSettings() override;

    /** Called when the visualizer's tab becomes visible again */
    void refreshState() override;

    /** Called instead of "repaint()" to avoid re-painting sub-components */
    void refresh() override;

    /** Draws the canvas background */
    void paint (Graphics& g) override;

    /** Called when acquisition starts to allow it to start any rendering. */
    void beginAnimation() override;

    /** Called when acquisition stops to allow it to stop any rendering. */
    void endAnimation() override;

private:
    QualityMonitor* processor;

    Array<ProbeMetrics> localMetrics;
    int selectedProbe = 0;

    // Sidebar — JUCE ListBox
    std::unique_ptr<ProbeListModel> probeListModel;
    std::unique_ptr<ListBox> probeListBox;

    // Scrollable content area
    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<ContentComponent> content;

    // Header controls
    std::unique_ptr<Label> durationLabel;
    std::unique_ptr<ComboBox> durationCombo;
    std::unique_ptr<ToggleButton> autoStartBtn;
    std::unique_ptr<TextButton> captureBtn;
    std::unique_ptr<TextButton> saveBtn;
    std::unique_ptr<Button> layoutGridBtn;
    std::unique_ptr<Button> layoutHStackBtn;
    std::unique_ptr<Button> layoutVStackBtn;
    std::unique_ptr<Label> statusIndicator;

    bool acquisitionActive = false;
    bool processingDone = false;
    int snapRefreshCounter = 0; // throttle DataSnapshotPanel to 1 Hz
    uint32_t lastSeenGeneration = 0; // tracks metricsGeneration to skip no-op deep copies

    static constexpr int SIDEBAR_W = 240;
    static constexpr int HEADER_H = 36;

    void selectProbe (int idx);
    void layoutPanels();
    Parameter* getSelectedProbeParameter (const String& parameterName) const;
    void updatePanelParameterEditors();
    void updateSaveButtonState();
    void saveCurrentRunArtifacts();

    void startProcessing();
    void stopProcessing();

    /** Generates an assertion if this class leaks */
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QualityMonitorCanvas)
};

#endif // QUALITYMONITORCANVAS_H_DEFINED
