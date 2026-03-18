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

#ifndef VISUALIZERCANVAS_H_INCLUDED
#define VISUALIZERCANVAS_H_INCLUDED

#include <VisualizerWindowHeaders.h>

class VisualizerPlugin;

/**
* 
	Draws data in real time

*/
class VisualizerPluginCanvas : public Visualizer
{
public:
    /** Constructor */
    VisualizerPluginCanvas (VisualizerPlugin* processor);

    /** Destructor */
    ~VisualizerPluginCanvas();

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
    VisualizerPlugin* processor;

    /** Generates an assertion if this class leaks */
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualizerPluginCanvas);
};

#endif // SPECTRUMCANVAS_H_INCLUDED
