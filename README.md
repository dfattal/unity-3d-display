# Monado 3D Display — Unity Plugin

Unity plugin for rendering on eye-tracked 3D light field displays via the Monado OpenXR runtime. Works with any display that has a Monado-compatible display processor driver (Leia, sim_display, etc.).

## Table of Contents

- [Overview](#overview)
- [Requirements](#requirements)
- [Installing the Plugin](#installing-the-plugin)
- [Enabling the Feature](#enabling-the-feature)
- [Scene Setup](#scene-setup)
  - [Camera-Centric Mode (Recommended)](#camera-centric-mode-recommended)
  - [Display-Centric Mode](#display-centric-mode)
- [Stereo Tunables Reference](#stereo-tunables-reference)
- [2D UI Overlay](#2d-ui-overlay)
- [Editor Preview](#editor-preview)
- [Building Your App](#building-your-app)
  - [Windows Standalone](#windows-standalone)
  - [macOS Standalone](#macOS-standalone)
  - [Cross-Compiling (macOS Editor to Windows Build)](#cross-compiling-macos-editor-to-windows-build)
- [Deploying to End Users](#deploying-to-end-users)
- [Testing Without Hardware](#testing-without-hardware)
- [Troubleshooting](#troubleshooting)
- [Architecture](#architecture)

---

## Overview

The plugin intercepts Unity's OpenXR pipeline at the native layer to provide:

- **Eye-tracked stereo rendering** — Kooima asymmetric frustum projection from real-time eye positions
- **Two stereo rig modes** — Camera-centric (add to existing camera) or display-centric (place a virtual display in the scene)
- **2D UI overlay** — Route any Canvas to a window-space composition layer with stereo disparity
- **Editor preview** — Side-by-side stereo preview without leaving the editor

The plugin works by hooking `xrLocateViews` before Unity sees the results, replacing the runtime's FOV data with Kooima-computed asymmetric frustums. Unity then builds correct projection matrices through its normal rendering pipeline — no `Camera.SetStereoProjectionMatrix` hacks required.

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **Unity** | 2022.3 LTS or later (including Unity 6) |
| **OpenXR Plugin** | `com.unity.xr.openxr` 1.9.1+ (installed via Package Manager) |
| **XR Plugin Management** | `com.unity.xr.management` 4.4.0+ (auto-installed with OpenXR) |
| **Monado Runtime** | Pre-built from openxr-3d-display CI — see [Deploying to End Users](#deploying-to-end-users) |
| **Display Driver** | Leia SR SDK for physical hardware, or `sim_display` for testing without a display |

### Platform Support

| Editor Platform | Build Target | Native Plugin | Status |
|----------------|--------------|---------------|--------|
| Windows | Windows x64 | `monado3d_unity.dll` | Supported |
| macOS | macOS | `libmonado3d_unity.dylib` | Supported |
| macOS | Windows x64 | `monado3d_unity.dll` | Supported (cross-compile) |

---

## Installing the Plugin

### Option A: From Git URL (Recommended)

1. In Unity: **Window > Package Manager**
2. Click **+** > **Add package from git URL...**
3. Enter:
   ```
   https://github.com/dfattal/unity-3d-display.git
   ```

### Option B: From Local Folder (Development)

1. Clone the repository:
   ```bash
   git clone https://github.com/dfattal/unity-3d-display.git
   ```
2. In Unity: **Window > Package Manager**
3. Click **+** (top-left) > **Add package from disk...**
4. Navigate to `unity-3d-display/package.json` and select it

After installation, the package appears as **Monado 3D Display** in the Package Manager.

---

## Enabling the Feature

1. Go to **Edit > Project Settings > XR Plug-in Management**
2. Under the **Standalone** tab (Windows or macOS), check **OpenXR**
3. Click the gear icon next to OpenXR (or expand **OpenXR > Features**)
4. Enable **Monado 3D Display**

You can verify the runtime connection under **Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display** — a status panel shows whether `XR_RUNTIME_JSON` is set and the display driver is detected.

---

## Scene Setup

### Camera-Centric Mode (Recommended)

This is the simplest setup. Your existing Main Camera stays in place and becomes the nominal viewing position. The plugin creates stereo eye offsets around it.

**Step 1:** Select your **Main Camera** in the hierarchy.

**Step 2:** Add Component > **Monado3DCamera** (found under Monado3D category).

That's it. The component:
- Reads eye tracking data from the runtime each frame
- Computes Kooima asymmetric frustums around the camera position
- Pushes modified FOVs into the OpenXR pipeline before Unity renders

```
Hierarchy:
  Main Camera              <-- has Camera + Monado3DCamera
    ├── Scene objects...
    └── (everything else in your scene)
```

**Inspector tunables:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| IPD Factor | 1.0 | Scales inter-eye distance. <1 = reduced stereo, >1 = exaggerated |
| Parallax Factor | 1.0 | Scales eye X/Y offset from viewing axis |
| Convergence Distance | auto | Distance to the virtual screen plane (meters). Auto-set from display info if 0. |
| Field of View | auto | Computed from convergence + display size. Override with non-zero value (degrees). |

**When to use camera-centric mode:**
- First-person games and apps
- Retrofitting stereo into an existing project (just add the component)
- When the camera moves freely through the scene

### Display-Centric Mode

The display is a fixed object in the scene. Eye positions are transformed relative to this virtual display.

**Step 1:** Create an empty GameObject where the virtual display should be:
- **GameObject > Create Empty**, name it "VirtualDisplay"
- Position and orient it in the scene (e.g., at `(0, 1.5, 2)` facing the player)

**Step 2:** Add Component > **Monado3DDisplay**

**Step 3:** Make your Main Camera a child of this object, or position it separately — the display's world transform is sent to the native plugin as the "scene transform" applied to raw eye positions.

```
Hierarchy:
  VirtualDisplay           <-- has Monado3DDisplay
    └── Main Camera        <-- standard Camera component
```

**Inspector tunables:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| IPD Factor | 1.0 | Scales inter-eye distance |
| Parallax Factor | 1.0 | Scales eye X/Y offset from display center |
| Perspective Factor | 1.0 | Scales eye Z only (depth intensity without changing baseline) |
| Scale Factor | 1.0 | Virtual display size relative to physical display (affects Kooima screen extents) |

**When to use display-centric mode:**
- Digital signage, museum exhibits, kiosks
- The virtual display is a physical object in the scene (e.g., a TV on a wall)
- You want the display's position and orientation to be an explicit scene element

---

## Stereo Tunables Reference

Both modes share a common tunable interface that maps to the native plugin's Kooima computation. Here's what each parameter does physically:

### IPD Factor (both modes)
Scales the horizontal distance between left and right eye positions.
- `1.0` = natural inter-pupillary distance from eye tracker
- `0.5` = half the real IPD (gentler stereo, less eye strain)
- `2.0` = double the real IPD (exaggerated depth, "macro" effect)
- `0.0` = mono rendering (both eyes at center)

### Parallax Factor (both modes)
Scales the eye's X and Y offset from the viewing axis (or display center).
- `1.0` = natural head parallax
- `0.0` = no parallax (stereo still works via IPD, but no motion parallax)
- `>1.0` = exaggerated motion parallax

### Perspective Factor (display-centric only)
Scales the eye's Z position (distance from display) without changing the baseline.
- `1.0` = natural depth
- `<1.0` = eyes appear closer to display (stronger perspective)
- `>1.0` = eyes appear farther (flatter perspective)

### Scale Factor (display-centric only)
Scales the virtual display dimensions used in the Kooima projection.
- `1.0` = virtual display matches physical display size
- `2.0` = virtual display twice as large (scene appears smaller)
- `0.5` = virtual display half-size (scene appears larger)

### Convergence Distance (camera-centric only)
Distance from the camera to the virtual screen plane, in meters. Objects at this distance appear at the display surface; closer objects pop out, farther objects recede.
- Default: auto-set from the display's `nominalViewerZ` (~0.5m for typical displays)
- Increase for deeper scenes, decrease for tabletop/near-field content

### Field of View (camera-centric only)
Vertical FOV override in degrees. When 0, auto-computed from convergence distance and physical display height. Override when you need a specific FOV regardless of display geometry.

---

## 2D UI Overlay

Route a Canvas to a window-space composition layer that the Monado compositor overlays on both eyes with per-eye disparity shift (renders pre-interlace).

1. Create a **Canvas** (any render mode)
2. Add Component > **Monado3DWindowSpaceUI** to the Canvas
3. Configure position and size in fractional window coordinates [0..1]:

| Parameter | Default | Description |
|-----------|---------|-------------|
| Position X | 0.0 | Left edge of overlay in window (0 = left, 1 = right) |
| Position Y | 0.0 | Bottom edge (0 = bottom, 1 = top) |
| Width | 1.0 | Fractional width |
| Height | 1.0 | Fractional height |
| Disparity | 0.0 | Stereo disparity in pixels. 0 = at screen plane, positive = in front |
| Resolution | 512 | Render texture resolution (square) |

The overlay is submitted as `XrCompositionLayerWindowSpaceEXT` and composited by Monado before display processing. This means 2D UI text stays sharp and is not interlaced — ideal for HUDs, menus, and status displays.

---

## Editor Preview

### Side-by-Side Preview (no runtime needed)

1. **Window > Monado3D > Preview Window**
2. Select **SideBySide** mode
3. The window shows a stereo pair computed from your scene cameras

This works without the Monado runtime running — useful for authoring and layout.

### Runtime Readback Preview (actual display processing)

1. Add a **Monado3DPreview** component to the same GameObject as your camera
2. In the Preview Window, select **RuntimeReadback** mode
3. The preview shows the actual composited + display-processed output

Requires Monado runtime running. Use this for final QA verification.

---

## Building Your App

### Windows Standalone

1. **File > Build Settings**
2. Select **Windows, Mac, Linux** platform
3. Set **Target Platform** to **Windows** and **Architecture** to **x86_64**
4. Verify in **Player Settings > XR Plug-in Management > Standalone** that OpenXR is enabled with the Monado 3D Display feature
5. Click **Build** or **Build And Run**

The build output includes `monado3d_unity.dll` in the `Plugins/` folder alongside your executable.

### macOS Standalone

1. **File > Build Settings**
2. Select **macOS** platform
3. Verify OpenXR + Monado 3D Display feature enabled in Standalone XR settings
4. Click **Build**

The `.app` bundle includes `libmonado3d_unity.dylib` in the plugins folder.

### Cross-Compiling (macOS Editor to Windows Build)

**Yes, you can build a Windows app from Unity for Mac.** Unity's cross-compilation support handles this:

1. Install the **Windows Build Support** module in Unity Hub:
   - Unity Hub > Installs > your Unity version > Add Modules > **Windows Build Support (Mono)**
2. In Unity: **File > Build Settings > Windows, Mac, Linux**
3. Set **Target Platform** to **Windows**
4. Build as normal

The plugin includes both platform binaries (`monado3d_unity.dll` for Windows, `libmonado3d_unity.dylib` for macOS). Unity automatically selects the correct one based on the build target.

**Important:** The built Windows `.exe` still requires the Monado runtime installed on the **target Windows machine** — see [Deploying to End Users](#deploying-to-end-users). You cannot run the Windows build on macOS.

---

## Deploying to End Users

Your built app is a standard OpenXR application. It needs an OpenXR runtime on the target machine.

### Windows Deployment

The end user's machine needs:

1. **Monado Runtime** — Install via the `SRMonadoInstaller.exe` from the openxr-3d-display CI build artifact (registers the runtime JSON and copies DLLs system-wide)

   Or, for development/testing, set the environment variable:
   ```cmd
   set XR_RUNTIME_JSON=C:\path\to\openxr_monado-dev.json
   ```

2. **Display Driver** — For physical 3D displays, install the vendor's SR SDK (e.g., LeiaSR). This sets `LEIASR_SDKROOT` and provides the eye tracking + display processing pipeline.

   For testing without hardware, set:
   ```cmd
   set SIM_DISPLAY_ENABLE=1
   set SIM_DISPLAY_OUTPUT=sbs
   ```
   (Options: `sbs`, `anaglyph`, `blend`)

### macOS Deployment

1. **Monado Runtime** — Set before launching the app:
   ```bash
   export XR_RUNTIME_JSON=/path/to/SRMonado-macOS/share/openxr/1/openxr_monado.json
   ```

2. **Display Driver** — Same as Windows: physical display SDK or sim_display:
   ```bash
   export SIM_DISPLAY_ENABLE=1
   export SIM_DISPLAY_OUTPUT=sbs
   ```

### What Happens Without the Runtime

If the Monado runtime is not installed, Unity's OpenXR loader fails to find a runtime and logs:
```
[OpenXR] No OpenXR runtime found
```
The `Monado3DFeature` logs a warning but doesn't crash — your app runs in mono (non-stereo) mode. You can check `Monado3DFeature.Instance` being null to detect this and show a user-facing message.

---

## Testing Without Hardware

The **sim_display** driver provides a software 3D display for development:

```bash
# Windows
set SIM_DISPLAY_ENABLE=1
set SIM_DISPLAY_OUTPUT=sbs          # side-by-side stereo
# or: anaglyph (red-cyan), blend (50/50 alpha)

# macOS
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT=sbs
```

With sim_display:
- Eye tracking is simulated via keyboard/mouse (qwerty driver)
- Display dimensions are synthetic (configurable)
- Output renders as SBS, anaglyph, or alpha-blend in a regular window

This lets you develop and test the full stereo pipeline on any machine.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No OpenXR runtime found" | `XR_RUNTIME_JSON` not set or points to missing file | Set the env var to the Monado runtime JSON path |
| Black screen | Monado 3D Display feature not enabled | Check Project Settings > XR Plug-in Management > OpenXR > Features |
| No stereo (flat image) | Eye tracking not running | Verify display driver (SR SDK) is installed, or enable sim_display |
| Stereo looks wrong | Tunables misconfigured | Reset to defaults (IPD=1, Parallax=1, Scale=1) |
| `DllNotFoundException: monado3d_unity` | Native plugin not found by Unity | Ensure the plugin binaries are in `Runtime/Plugins/Windows/x64/` or `Runtime/Plugins/macOS/` |
| HDRP stereo artifacts | Single-pass instanced issue | Verify both eye views have correct FOVs in Frame Debugger |
| Editor preview blank | No preview component or wrong mode | Add Monado3DPreview component; use SideBySide mode without runtime |
| `VK_ERROR_EXTENSION_NOT_PRESENT` on macOS | MoltenVK limitation | Known issue — use sim_display or physical display driver |

### Debug Logging

Enable **Log Eye Tracking** on the Monado3DCamera or Monado3DDisplay component to see per-frame eye positions in the Console:
```
[Monado3D] Eyes: L=(0.032, 0.001, 0.504), R=(-0.031, 0.001, 0.504), tracked=True
```

### Checking Runtime Status in Editor

**Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display** shows:
- Runtime JSON path and whether the file exists
- Display driver status (Leia SR SDK or sim_display)
- Connected display properties (resolution, physical size, nominal viewer distance)
- Eye tracking status

---

## Architecture

```
Unity Editor / Player
┌──────────────────────────────────────────────────────────┐
│  C# Layer                                                │
│                                                          │
│  Monado3DFeature : OpenXRFeature                         │
│    HookGetInstanceProcAddr → install native hooks        │
│    OnSystemChange → query XrDisplayInfoEXT               │
│    API: SetTunables(), SetSceneTransform()               │
│                                                          │
│  Monado3DCamera          Monado3DDisplay                 │
│  (camera-centric)        (display-centric)               │
│  Attach to Camera        Place in scene                  │
│                                                          │
│  Monado3DWindowSpaceUI   Monado3DPreview                 │
│  (2D overlay)            (editor preview)                │
└──────────────────────────────────────────────────────────┘
        │ P/Invoke (monado3d_unity.dll / .dylib)
        ▼
┌──────────────────────────────────────────────────────────┐
│  Native Plugin (C/C++)                                   │
│  Hook chain:                                             │
│    xrLocateViews → scene transform → tunables → Kooima  │
│    xrCreateSession → inject window binding               │
│    xrGetSystemProperties → extract display info          │
│    xrEndFrame → submit overlay layers                    │
│  Thread-safe double-buffered shared state                │
└──────────────────────────────────────────────────────────┘
        │ Standard OpenXR API
        ▼
┌──────────────────────────────────────────────────────────┐
│  Monado Runtime                                          │
│  Compositor → Display Processor → Output                 │
└──────────────────────────────────────────────────────────┘
```

### Transform Chain (per frame)

```
Eye Tracker → raw positions in DISPLAY space
    ↓
Scene Transform (parent camera pose, zoom)
    ↓
Tunables (IPD factor, parallax, perspective, scale)
    ↓
Kooima Asymmetric Frustum → XrFovf angles
    ↓
Unity builds projection matrices → renders stereo
```

---

## Package Contents

| Path | Purpose |
|------|---------|
| `Runtime/Monado3DFeature.cs` | OpenXR Feature — lifecycle hooks, P/Invoke, public API |
| `Runtime/Monado3DCamera.cs` | Camera-centric stereo rig MonoBehaviour |
| `Runtime/Monado3DDisplay.cs` | Display-centric stereo rig MonoBehaviour |
| `Runtime/Monado3DDisplayInfo.cs` | Display properties data struct |
| `Runtime/Monado3DTunables.cs` | Tunable parameters struct |
| `Runtime/Monado3DWindowSpaceUI.cs` | 2D UI overlay routing |
| `Runtime/Monado3DPreview.cs` | Editor preview (SBS + readback) |
| `Runtime/Monado3DNative.cs` | P/Invoke bindings to native plugin |
| `Runtime/Plugins/Windows/x64/` | Windows native plugin (DLL) |
| `Runtime/Plugins/macOS/` | macOS native plugin (dylib) |
| `Editor/Monado3DDisplayEditor.cs` | Custom inspector for display-centric mode |
| `Editor/Monado3DCameraEditor.cs` | Custom inspector for camera-centric mode |
| `Editor/Monado3DPreviewWindow.cs` | Editor preview window |
| `Editor/Monado3DSettingsProvider.cs` | Project Settings page |
| `native~/` | Native C/C++ plugin source + CMakeLists.txt |
