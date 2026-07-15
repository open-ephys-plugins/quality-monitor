# Quality Monitor

Monitors the health of continuous electrophysiology signals in real-time.

## Installation

This plugin can be added via the Open Ephys GUI Plugin Installer. To access the Plugin Installer, press **ctrl-P** or **⌘P** from inside the GUI. Once the installer is loaded, browse to the "Quality Monitor" plugin and click "Install."

## Usage

Instructions for using the Quality Monitor plugin are available [here](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Quality-Monitor.html).


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

**Requirements:** [Visual Studio 2022 (or higher)](https://visualstudio.microsoft.com/) and
[CMake >=3.15](https://cmake.org/install/)

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

**Requirements:** [CMake >=3.15](https://cmake.org/install/)

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
