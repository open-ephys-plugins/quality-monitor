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

#include "VisualizerPluginEditor.h"

#include "VisualizerPlugin.h"
#include "VisualizerPluginCanvas.h"

VisualizerPluginEditor::VisualizerPluginEditor (GenericProcessor* p)
    : VisualizerEditor (p, "Visualizer", 240)
{
    // Add parameter editors here. Note the parameters
    // must be created in the processor's registerParameters() method.
    // For example:
    // addSelectedChannelsParameterEditor(Parameter::STREAM_SCOPE,
    //                                    "channels",
    //                                    15, 40);
}

Visualizer* VisualizerPluginEditor::createNewCanvas()
{
    return new VisualizerPluginCanvas ((VisualizerPlugin*) getProcessor());
    ;
}
