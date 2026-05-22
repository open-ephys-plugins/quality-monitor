# Quality Monitor

Open Ephys GUI visualizer plugin for monitoring ephys data health in real time. It summarizes RMS noise, powerline and high-frequency spectral content, raw data snapshots, and spike-rate activity across all channels for each detected probe.

## Installation

If a release is available for your platform, install Quality Monitor through the
Open Ephys GUI Plugin Installer. Open **File > Plugin Installer** or press
**Ctrl+P** (**Cmd+P** on macOS), search for **Quality Monitor**, and install the
desired version.

If the plugin is not listed for your platform, build it from source using the
instructions below.

## Usage

Add Quality Monitor downstream of a Neuropixels signal source, then open the
plugin canvas to inspect each probe. The left sidebar lists detected probes, and
the main view shows four diagnostics for the selected probe:

- **RMS Heatmap** for channel-by-channel noise over time
- **Power Spectrum** for powerline contamination and broadband noise
- **Data Snapshot** for short raw-voltage excerpts and saturation checks
- **Spike Rate** for cumulative and live firing-rate estimates

Use the editor to select the local powerline frequency (**50 Hz** or **60 Hz**).
In the canvas, choose an analysis duration, enable **Auto Start** if you want
analysis to begin with acquisition, or use **Capture** and **Stop** to control
runs manually. Thresholds for RMS, powerline SNR, and spike-rate alerts can be
adjusted per probe from the panel headers. Once a run completes, **Save** exports
panel snapshots and a metrics JSON summary.


## Building from source

First, follow the instructions on the [Open Ephys GUI developer
docs](https://open-ephys.github.io/gui-docs/Developer-Guide/Compiling-the-GUI.html)
to build the Open Ephys GUI.

This plugin is intended to live alongside the `plugin-GUI` repository:

```text
Code/
├── plugin-GUI
│   ├── Build
│   ├── Plugins
│   └── ...
└── OEPlugins
    └── quality-monitor
        ├── Build
        ├── Source
        └── ...
```

Quality Monitor uses FFTW3. CMake is configured to look for platform-specific
headers and libraries under `libs/` during configuration.

### Windows

**Requirements:** [Visual Studio 2022](https://visualstudio.microsoft.com/) and
[CMake](https://cmake.org/install/)

From the `Build` directory, run:

```bash
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
cmake --install . --config Release
```

This builds the plugin DLL and copies it into the Open Ephys GUI `plugins`
directory. You can also open the generated Visual Studio solution from `Build/`
and build the `INSTALL` target there.

### Linux

**Requirements:** [CMake](https://cmake.org/install/)

From the `Build` directory, run:

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j4
cmake --install .
```

This builds the plugin `.so` and installs it into the compiled GUI's `plugins`
directory.

### macOS

**Requirements:** [Xcode](https://developer.apple.com/xcode/) and
[CMake](https://cmake.org/install/)

From the `Build` directory, run:

```bash
cmake -G "Xcode" ..
cmake --build . --config Release
cmake --install . --config Release
```

This builds the plugin bundle and installs it to
`~/Library/Application Support/open-ephys/plugins-api10`.
