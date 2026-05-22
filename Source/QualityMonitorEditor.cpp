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
#include <UIUtilitiesHeaders.h>

class PowerlineHzParameterEditor : public ParameterEditor,
                                   public Button::Listener
{
public:
    PowerlineHzParameterEditor (Parameter* param) : ParameterEditor (param)
    {
        powerline50HzButton = new TextButton ("50 Hz", "Output to left channel only");
        powerline50HzButton->setClickingTogglesState (true);
        powerline50HzButton->setToggleState (false, dontSendNotification);

        powerline60HzButton = new TextButton ("60 Hz", "Output to right channel only");
        powerline60HzButton->setClickingTogglesState (true);
        powerline60HzButton->setToggleState (false, dontSendNotification);

        powerlineButtonManager = std::make_unique<LinearButtonGroupManager>();
        powerlineButtonManager->addButton (powerline50HzButton);
        powerlineButtonManager->addButton (powerline60HzButton);
        powerlineButtonManager->setRadioButtonMode (true);
        powerlineButtonManager->setButtonListener (this);
        powerlineButtonManager->setSelectedButtonIndex (1);
        powerline60HzButton->setToggleState (true, dontSendNotification);
        addAndMakeVisible (powerlineButtonManager.get());

        powerlineLabel = std::make_unique<Label> ("Powerline Label", "Powerline Freq.");
        powerlineLabel->setFont (FontOptions ("Inter", "Regular", 13.5f));
        powerlineLabel->setJustificationType (Justification::centredLeft);
        addAndMakeVisible (powerlineLabel.get());

        setBounds (0, 0, 180, 18);
        powerlineButtonManager->setBounds (0, 0, 88, 18);
        powerlineLabel->setBounds (92, 0, 88, 18);
    }

    void buttonClicked (Button* buttonThatWasClicked) override
    {
        const String buttonName = buttonThatWasClicked->getName().toLowerCase();

        if (buttonName.startsWith ("50"))
        {
            param->setNextValue (50.0f);
        }
        else if (buttonName.startsWith ("60"))
        {
            param->setNextValue (60.0f);
        }
    }

    void updateView() override
    {
        if (param != nullptr)
        {
            float value = dynamic_cast<FloatParameter*> (param)->getFloatValue();
            if (value == 50.0f)
            {
                powerline50HzButton->setToggleState (true, dontSendNotification);
                powerline60HzButton->setToggleState (false, dontSendNotification);
                powerlineButtonManager->setSelectedButtonIndex (0);
            }
            else if (value == 60.0f)
            {
                powerline60HzButton->setToggleState (true, dontSendNotification);
                powerline50HzButton->setToggleState (false, dontSendNotification);
                powerlineButtonManager->setSelectedButtonIndex (1);
            }
        }
    }

private:
    TextButton* powerline50HzButton;
    TextButton* powerline60HzButton;
    std::unique_ptr<LinearButtonGroupManager> powerlineButtonManager;
    std::unique_ptr<Label> powerlineLabel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerlineHzParameterEditor)
};

QualityMonitorEditor::QualityMonitorEditor (GenericProcessor* p)
    : VisualizerEditor (p, "Quality Monitor", 200)
{
    addMaskChannelsParameterEditor (Parameter::STREAM_SCOPE, QualityMonitorParams::kMaskedChannelsParam, 15, 45);
    auto* channelsEditor = getParameterEditor (QualityMonitorParams::kMaskedChannelsParam);
    channelsEditor->setSize (180, 18);

    Parameter* powerlineParam = p->getParameter (QualityMonitorParams::kPowerlineHzParam);
    addCustomParameterEditor (new PowerlineHzParameterEditor (powerlineParam), 15, 85);
}

Visualizer* QualityMonitorEditor::createNewCanvas()
{
    return new QualityMonitorCanvas ((QualityMonitor*) getProcessor());
}
