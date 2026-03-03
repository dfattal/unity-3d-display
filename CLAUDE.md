# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Project Overview

Unity plugin for eye-tracked 3D light field displays via the **Monado OpenXR runtime**. This is a Unity Package Manager (UPM) package that intercepts Unity's OpenXR pipeline at the native layer to provide Kooima asymmetric frustum projection for stereo rendering.

The plugin works with the **openxr-3d-display** runtime ([dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)) but has **no source dependency** on it — native code fetches OpenXR headers independently from Khronos.

## Repository Structure

This repo root IS the UPM package root (`package.json` is at the top level).

```
unity-3d-display/              # repo root = UPM package root
├── package.json               # UPM manifest
├── Runtime/                   # C# runtime scripts + native plugin binaries
│   ├── *.cs                   # MonoBehaviours and OpenXR Feature
│   └── Plugins/               # Built native binaries (Windows/macOS)
├── Editor/                    # C# editor-only scripts (inspectors, settings)
├── Samples~/                  # UPM samples (lazy-imported by user)
├── native~/                   # Native C/C++ plugin source (not imported by Unity)
│   └── CMakeLists.txt         # Independent CMake build
└── .github/workflows/         # CI for building native plugin
```

## Building the Native Plugin

```bash
cd native~
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Output: `monado3d_unity.dll` (Windows) or `libmonado3d_unity.dylib` (macOS).

Copy built binary to `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/`.

### Cross-compiling for Windows on macOS

```bash
cd native~
mkdir build-win && cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Key Architecture

### Three Layers

1. **Runtime (C#)** — `Monado3DFeature.cs` hooks into OpenXR lifecycle; `Monado3DCamera.cs` and `Monado3DDisplay.cs` are the two stereo rig modes
2. **Editor (C#)** — Custom inspectors, preview window, settings page
3. **Native (C/C++)** — Hook chain on `xrLocateViews`, `xrCreateSession`, `xrGetSystemProperties`, `xrEndFrame`; Kooima projection math; thread-safe shared state

### OpenXR Hook Chain

The native plugin intercepts OpenXR calls via `xrGetInstanceProcAddr` hooking:
- `xrLocateViews` → applies scene transform + tunables + Kooima projection
- `xrCreateSession` → injects window binding extension
- `xrGetSystemProperties` → extracts display info
- `xrEndFrame` → submits overlay composition layers

### Wire Protocol

Extension struct definitions in `native~/monado3d_extensions.h` must match the runtime's implementation. These are versioned by extension spec version. When changing extensions, update the runtime first, then the plugin.

## Development Workflow

### Testing in Unity

1. Open any Unity 2022.3+ project
2. Add this package via Package Manager (local path or git URL)
3. Enable the feature: Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display
4. Set `XR_RUNTIME_JSON` environment variable to point to a Monado runtime build
5. For testing without hardware: `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs`

### Code Style

- C# follows Unity conventions (PascalCase for public members, camelCase for private)
- C/C++ follows the Monado coding style (snake_case, C11/C++17)
- Native code in `native~/` uses tabs for indentation

## Cross-Repo References

- Runtime repo: [dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)
- Use `dfattal/openxr-3d-display#N` syntax to reference runtime issues
- The runtime provides the OpenXR compositor, display drivers, and eye tracking
- The plugin provides the Unity-side stereo rendering pipeline
