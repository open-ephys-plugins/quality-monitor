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

#include "QualityMonitorEditor.h"

#include "QualityMonitor.h"
#include "QualityMonitorCanvas.h"

QualityMonitorEditor::QualityMonitorEditor (GenericProcessor* p)
    : VisualizerEditor (p, "Quality Monitor", 210)
{
    // Add parameter editors here. Note the parameters
    // must be created in the processor's registerParameters() method.
    // For example:
    // addSelectedChannelsParameterEditor(Parameter::STREAM_SCOPE,
    //                                    "channels",
    //                                    15, 40);
}

Visualizer* QualityMonitorEditor::createNewCanvas()
{
    return new QualityMonitorCanvas ((QualityMonitor*) getProcessor());
}
