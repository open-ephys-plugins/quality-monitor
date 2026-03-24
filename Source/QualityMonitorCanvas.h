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

#ifndef QUALITYMONITORCANVAS_H_DEFINED
#define QUALITYMONITORCANVAS_H_DEFINED

#include "ProbeMetrics.h"
#include <VisualizerWindowHeaders.h>
#include <functional>
#include <vector>

class QualityMonitor;

namespace QCColours
{
Colour inferno (float t);
Colour cividis (float t);
Colour statusCol (ProbeStatus s);
String statusStr (ProbeStatus s);
} // namespace QCColours

class RmsHeatmapPanel : public Component
{
public:
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;

private:
    std::vector<float> rmsUV;
    int numCh = 0;
    float threshUV = 20.0f;
    int numHighRms = 0;
    float maxRms = 1.0f;
    void drawColourBar (Graphics& g, Rectangle<float> r);
};

class PowerSpectrumPanel : public Component
{
public:
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;

private:
    std::vector<float> spectrum;
    int numCh = 0;
    float sampleRate = 30000.0f;
    float powerlineHz = 60.0f;
    int numNoisyCh = 0;
    static constexpr int NUM_TRACES = 6;
};

class DataSnapshotPanel : public Component
{
public:
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;

private:
    std::vector<float> snapshot;
    int numCh = 0;
    float minUV = -50.0f;
    float maxUV = 50.0f;
};

class SpikeRatePanel : public Component
{
public:
    void updateData (const ProbeMetrics& m);
    void paint (Graphics& g) override;

private:
    std::vector<float> rateHz;
    int numCh = 0;
    float spikeFailHz = 0.1f;
    float spikeLowHz = 2.0f;
    int numLowCh = 0;
    float maxRateHz = 30.0f;
    void drawLegend (Graphics& g, Rectangle<int> r);
};

class ProbeListRow : public Component
{
public:
    void setMetrics (const ProbeMetrics& m, bool selected);
    void paint (Graphics& g) override;
    void mouseDown (const MouseEvent&) override;
    std::function<void()> onClick;

private:
    String name;
    int numCh = 0;
    ProbeStatus status = ProbeStatus::UNKNOWN;
    bool sel = false;
};

/**
* 
	Draws data in real time

*/
class QualityMonitorCanvas : public Visualizer
{
public:
    /** Constructor */
    QualityMonitorCanvas (QualityMonitor* processor);

    /** Destructor */
    ~QualityMonitorCanvas();

    /** Updates boundaries of sub-components whenever the canvas size changes */
    void resized() override;

    /** Called by the update() method to allow the visualizer to update its custom settings.*/
    void updateSettings() override;

    /** Called when the visualizer's tab becomes visible again */
    void refreshState() override;

    /** Called instead of "repaint()" to avoid re-painting sub-components*/
    void refresh() override;

    /** Draws the canvas background */
    void paint (Graphics& g) override;

private:
    /** Pointer to the processor class */
    QualityMonitor* processor;

    Array<ProbeMetrics> localMetrics;
    OwnedArray<ProbeListRow> probeRows;
    int selectedProbe = 0;

    std::unique_ptr<RmsHeatmapPanel> rmsPanel;
    std::unique_ptr<PowerSpectrumPanel> specPanel;
    std::unique_ptr<DataSnapshotPanel> snapPanel;
    std::unique_ptr<SpikeRatePanel> spikePanel;

    std::unique_ptr<ComboBox> durationCombo;
    std::unique_ptr<TextButton> captureBtn;
    std::unique_ptr<TextButton> saveBtn;
    std::unique_ptr<Label> statusIndicator;

    static constexpr int SIDEBAR_W = 200;
    static constexpr int HEADER_H = 32;
    static constexpr int ROW_H = 56;
    static constexpr int REFRESH_HZ = 5;

    void rebuildProbeRows();
    void selectProbe (int idx);
    void layoutPanels();
    void paintSidebar (Graphics& g);
    void paintHeader (Graphics& g);

    /** Generates an assertion if this class leaks */
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QualityMonitorCanvas);
};

#endif // QUALITYMONITORCANVAS_H_INCLUDED
