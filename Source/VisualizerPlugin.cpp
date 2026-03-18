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

#include "VisualizerPlugin.h"

#include "VisualizerPluginEditor.h"

VisualizerPlugin::VisualizerPlugin()
    : GenericProcessor ("Visualizer Plugin")
{
}

VisualizerPlugin::~VisualizerPlugin()
{
}

AudioProcessorEditor* VisualizerPlugin::createEditor()
{
    editor = std::make_unique<VisualizerPluginEditor> (this);
    return editor.get();
}

void VisualizerPlugin::registerParameters()
{
    // Register any parameters here if needed
    // For example: addSelectedChannelsParameter (Parameter::STREAM_SCOPE,
    //                                 channels,
    //                                 Channels,
    //                                 Selected channels to use);
}

void VisualizerPlugin::updateSettings()
{
}

void VisualizerPlugin::process (AudioBuffer<float>& buffer)
{
    checkForEvents (true);
}

void VisualizerPlugin::handleTTLEvent (TTLEventPtr event)
{
}

void VisualizerPlugin::handleSpike (SpikePtr spike)
{
}

void VisualizerPlugin::handleBroadcastMessage (const String& message, const int64 systemTimeMillis)
{
}

void VisualizerPlugin::saveCustomParametersToXml (XmlElement* parentElement)
{
}

void VisualizerPlugin::loadCustomParametersFromXml (XmlElement* parentElement)
{
}
