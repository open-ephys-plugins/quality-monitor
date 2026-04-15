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

#ifndef COLORMAP_H_DEFINED
#define COLORMAP_H_DEFINED

#include <BasicJuceHeader.h>

namespace ColourMaps
{
Colour inferno (float t);
Colour cividis (float t);
Colour viridis (float t);
Colour turbo   (float t);
} // namespace ColourMaps

#endif // COLORMAP_H_DEFINED
