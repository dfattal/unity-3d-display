# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.

## Project Overview

Unity plugin for eye-tracked 3D light field displays via the **DisplayXR OpenXR runtime**. This is a Unity Package Manager (UPM) package that intercepts Unity's OpenXR pipeline at the native layer to provide Kooima asymmetric frustum projection for stereo rendering. The primary editor workflow is a **standalone preview window** that creates its own OpenXR session — no Play Mode needed.

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

Output: `displayxr_unity.dll` (Windows) or `libdisplayxr_unity.dylib` (macOS).

Copy built binary to `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/`.

### Local builds

**macOS (native, produces shipping binary):**
```bash
native~/build-mac.sh
```
Builds Universal binary (x86_64 + arm64) → `Runtime/Plugins/macOS/displayxr_unity.bundle`.

**Windows (MinGW cross-compile, compile check only):**
```bash
native~/build-win.sh
```
Verifies the code compiles for Windows but the DLL stays in `build-win/` (MinGW ABI, not shipped). The real Windows DLL comes from CI (MSVC-built) and is downloaded automatically by `/ci-monitor` after a successful build.

**Claude Code: After modifying any file in `native~/`, always run `native~/build-mac.sh` to update the local macOS binary, then run `native~/build-win.sh` as a compile check. Use `/ci-monitor` to commit, push, build via CI, and download the MSVC-built Windows DLL into `Runtime/Plugins/Windows/x64/`.**

## Key Architecture

### Three Layers

1. **Runtime (C#)** — `DisplayXRFeature.cs` hooks into OpenXR lifecycle; `DisplayXRCamera.cs` and `DisplayXRDisplay.cs` are the two stereo rig modes; `DisplayXRPreview.cs` provides inline preview textures (SBS, readback, SharedTexture)
2. **Editor (C#)** — Custom inspectors, settings page, and the standalone preview system (`DisplayXRPreviewSession.cs` manages an independent OpenXR session; `DisplayXRPreviewWindow.cs` provides the editor UI with camera selector and rendering mode controls)
3. **Native (C/C++)** — Hook chain on `xrLocateViews`, `xrCreateSession`, `xrGetSystemProperties`, `xrEndFrame`; Kooima projection math; thread-safe shared state

### Key Features

- **Two stereo rig modes**: Camera-centric (`DisplayXRCamera` — inherits camera FOV, inv. convergence distance tunable) and display-centric (`DisplayXRDisplay` — physical display geometry, virtual display height, scale-as-zoom)
- **Standalone editor preview**: Own OpenXR session bypassing Unity XR. Camera selector dropdown, dynamic rendering mode enumeration, zero-copy SharedTexture output (IOSurface/DXGI). Replaces Play Mode for DisplayXR workflows.
- **Play Mode conflict prevention**: Preview auto-removes Unity's OpenXR loader on Play entry, restores on exit (saved via SessionState)
- **2D UI overlay**: Canvas → `XrCompositionLayerWindowSpaceEXT` with stereo disparity
- **Native Kooima math**: `display3d_view.c` (screen-edge frustum) and `camera3d_view.c` (tangent-space frustum) — pure C, no DisplayXR dependency

### OpenXR Hook Chain

The native plugin intercepts OpenXR calls via `xrGetInstanceProcAddr` hooking:
- `xrLocateViews` → applies scene transform + tunables + Kooima projection
- `xrCreateSession` → injects window binding extension
- `xrGetSystemProperties` → extracts display info
- `xrEndFrame` → submits overlay composition layers

### Wire Protocol

Extension struct definitions in `native~/displayxr_extensions.h` must match the runtime's implementation. These are versioned by extension spec version. When changing extensions, update the runtime first, then the plugin.

## Development Workflow

### Testing in Unity

1. Open any Unity 2022.3+ project
2. Add this package via Package Manager (local path or git URL)
3. Enable the feature: Project Settings > XR Plug-in Management > OpenXR > DisplayXR
4. Set `XR_RUNTIME_JSON` environment variable to point to a DisplayXR runtime build (or use `SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=sbs` for testing without hardware)
5. **Open Window > DisplayXR > Preview Window, click Start** — this is the primary workflow
6. Play Mode still works but the standalone preview is preferred (avoids XR session conflicts)

### Critical: OpenXR Package Version

**You MUST use `com.unity.xr.openxr` version 1.16.1 or later.** The minimum version in `package.json` is 1.9.1 for broad compatibility, but versions before 1.16.1 ignore the `XR_RUNTIME_JSON` environment variable and the system `active_runtime.json` in editor play mode — they silently fall back to Unity's built-in Mock Runtime. This causes `0x0` display resolution, no IOSurface/shared texture, and no display info from the runtime. The failure is silent (no error logged, just mock runtime loaded). Always pin `1.16.1+` in your test project's `Packages/manifest.json`.

### Code Style

- C# follows Unity conventions (PascalCase for public members, camelCase for private)
- C/C++ follows the DisplayXR coding style (snake_case, C11/C++17)
- Native code in `native~/` uses tabs for indentation

## CI and Releases

### Day-to-day: just push to main
Every push to `main` that touches `native~/` triggers `build-native.yml`, which builds the DLL (Windows x64) and bundle (macOS Universal) and uploads them as CI artifacts. No tags, no releases. Artifacts are available for 90 days.

C#-only changes don't trigger the workflow (path filter).

### Creating a release (when ready)
Releases are triggered **only** by manually pushing a `v*` tag:
```bash
# 1. Make sure main is clean and CI passes
# 2. Bump version in package.json if needed
# 3. Update CHANGELOG.md with release notes
# 4. Tag and push:
git tag v0.1.0
git push origin v0.1.0
```

This triggers the `release` job in CI which:
- Builds both platform binaries
- Creates a `.tgz` UPM tarball with binaries included
- Pushes a `upm` branch with binaries committed (for git URL installs)
- Creates a `upm/v0.1.0` tag for version-pinned installs
- Publishes a GitHub Release with changelog notes and the `.tgz` attached

### Fixing a bad release
Tags are cheap and deletable:
```bash
git tag -d v0.1.0                     # delete local
git push origin :refs/tags/v0.1.0     # delete remote
# delete the GitHub Release in the web UI
# fix the issue, then re-tag
```

### Install paths for users

| Method | URL | Notes |
|--------|-----|-------|
| Git URL (latest release) | `...git#upm` | Tracks latest release |
| Git URL (pinned version) | `...git#upm/v0.1.0` | For production |
| Tarball | Download `.tgz` from Releases page | Offline installs |
| Local dev | Clone repo + build `native~/` yourself | For contributors |

The `upm` branch and release tarball only exist after the first `v*` tag is pushed.

## Claude Code Skills

### /ci-monitor - Automated Build Workflow
Automates the complete CI workflow for native plugin builds: commit → push → monitor → auto-fix.

**Usage:**
```bash
/ci-monitor "commit message"    # Commit with message and monitor build
/ci-monitor                      # Auto-generate commit message from changes
/ci-monitor --watch-only         # Just monitor current build without committing
```

**Features:**
- Launches subagent to preserve main conversation context
- Monitors GitHub Actions `build-native.yml` (Windows x64 + macOS Universal)
- Auto-diagnoses common build errors (missing includes, undeclared identifiers, linker errors)
- Attempts up to 3 automatic fixes before reporting failure
- Detects when only C# files changed (no native build triggered)

**Skill location:** `.claude/skills/ci-monitor/SKILL.md`

## Cross-Repo References

- Runtime repo: [dfattal/openxr-3d-display](https://github.com/dfattal/openxr-3d-display)
- Use `dfattal/openxr-3d-display#N` syntax to reference runtime issues
- The runtime provides the OpenXR compositor, display drivers, and eye tracking
- The plugin provides the Unity-side stereo rendering pipeline
