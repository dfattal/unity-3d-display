# Quick Start Guide

Step-by-step walkthrough: install the plugin, build two demo scenes, test both stereo rig modes, build standalone apps for Windows and macOS, and verify the pipeline end-to-end.

**Time estimate:** ~30 minutes (less if you already have a Unity project open).

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Step 1: Install the Plugin](#step-1-install-the-plugin)
  - [macOS](#macos-install)
  - [Windows](#windows-install)
- [Step 2: Enable the Feature](#step-2-enable-the-feature)
- [Step 3: Set Up the Runtime](#step-3-set-up-the-runtime)
- [Step 4: Demo Scene A — Camera-Centric Mode](#step-4-demo-scene-a--camera-centric-mode)
- [Step 5: Demo Scene B — Display-Centric Mode](#step-5-demo-scene-b--display-centric-mode)
- [Step 6: Test in the Editor](#step-6-test-in-the-editor)
- [Step 7: Build a Standalone App](#step-7-build-a-standalone-app)
  - [Windows Build](#windows-build)
  - [macOS Build](#macos-build)
  - [Cross-Compiling (macOS to Windows)](#cross-compiling-macos-to-windows)
- [Step 8: Run the Built App](#step-8-run-the-built-app)
- [What to Look For](#what-to-look-for)
- [Next Steps](#next-steps)

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **Unity** | 2022.3 LTS or later (including Unity 6). Install via [Unity Hub](https://unity.com/download). |
| **Build Support Modules** | In Unity Hub, add modules for your target platform(s): **Windows Build Support (Mono)** and/or **macOS Build Support (Mono)**. |
| **Monado Runtime** | Either a hardware-connected runtime from [openxr-3d-display](https://github.com/dfattal/openxr-3d-display), or the sim_display driver for testing without hardware. |

You do **not** need a 3D display to complete this guide — the sim_display driver provides a software display for development.

---

## Step 1: Install the Plugin

### macOS Install

1. Open (or create) a Unity project — any render pipeline (Built-in, URP, HDRP) works.

2. Clone the repository (if you haven't already):
   ```bash
   git clone https://github.com/dfattal/unity-3d-display.git
   ```

3. Build the native plugin:
   ```bash
   cd unity-3d-display/native~
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   ```
   This produces `Runtime/Plugins/macOS/monado3d_unity.bundle`.

4. In Unity: **Window > Package Manager**.

5. Click the **+** button (top-left) and choose **Add package from disk...**

6. Navigate to the cloned `unity-3d-display/package.json` and select it.

7. The package appears as **Monado 3D Display** in the Package Manager list.

> **After a release is published:** Once a `v*` tag has been pushed (creating the `upm` branch with pre-built binaries), you can skip the clone/build steps and install directly via git URL: **Add package from git URL...** → `https://github.com/dfattal/unity-3d-display.git#upm`. See the [README](../README.md#installing-the-plugin) for all install methods.

### Windows Install

1. Open (or create) a Unity project.

2. Clone the repository:
   ```cmd
   git clone https://github.com/dfattal/unity-3d-display.git
   ```

3. Build the native plugin (requires [CMake](https://cmake.org/download/) and Visual Studio or Build Tools):
   ```cmd
   cd unity-3d-display\native~
   mkdir build && cd build
   cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release
   ```
   This produces `Runtime\Plugins\Windows\x64\monado3d_unity.dll`.

4. In Unity: **Window > Package Manager > + > Add package from disk...**

5. Select the cloned `unity-3d-display\package.json`.

6. The package appears as **Monado 3D Display**.

> **Note:** If your machine doesn't have `git` on PATH, install [Git for Windows](https://git-scm.com/download/win) first.
>
> **After a release is published:** Use **Add package from git URL...** → `https://github.com/dfattal/unity-3d-display.git#upm` to install with pre-built binaries (no local build needed). Or download the `.tgz` from the [Releases page](https://github.com/dfattal/unity-3d-display/releases) and use **Add package from tarball...**

---

## Step 2: Enable the Feature

This step is the same on both platforms:

1. Go to **Edit > Project Settings > XR Plug-in Management**.

2. Under the **Standalone** tab (desktop icon), check **OpenXR**.
   - Unity will install the OpenXR package if not already present.

3. Under **OpenXR**, expand the **Features** list (or click the gear icon next to OpenXR).

4. Check **Monado 3D Display**.

5. Verify by scrolling to **Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display** — you should see a status panel showing whether `XR_RUNTIME_JSON` is configured.

---

## Step 3: Set Up the Runtime

You need an OpenXR runtime active for the plugin to connect. For development without hardware, use the **sim_display** driver:

### macOS

Open Terminal **before** launching Unity (environment variables must be set in the process that spawns Unity):

```bash
# Point to your Monado runtime build
export XR_RUNTIME_JSON=/path/to/SRMonado-macOS/share/openxr/1/openxr_monado.json

# Enable simulated display (skip if you have real hardware)
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT=sbs

# Launch Unity from this terminal
open -a "Unity Hub"
# Or launch Unity directly:
# /Applications/Unity/Hub/Editor/2022.3.XXf1/Unity.app/Contents/MacOS/Unity -projectPath /path/to/your/project
```

### Windows

Open Command Prompt or PowerShell **before** launching Unity:

**Command Prompt:**
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_monado-dev.json

set SIM_DISPLAY_ENABLE=1
set SIM_DISPLAY_OUTPUT=sbs

start "" "C:\Program Files\Unity Hub\Unity Hub.exe"
```

**PowerShell:**
```powershell
$env:XR_RUNTIME_JSON = "C:\path\to\openxr_monado-dev.json"
$env:SIM_DISPLAY_ENABLE = "1"
$env:SIM_DISPLAY_OUTPUT = "sbs"

& "C:\Program Files\Unity Hub\Unity Hub.exe"
```

> **Tip:** On Windows, if the Monado runtime was installed system-wide via `SRMonadoInstaller.exe`, you don't need to set `XR_RUNTIME_JSON` — it's already registered.

---

## Step 4: Demo Scene A — Camera-Centric Mode

Camera-centric mode works like a standard Unity camera with added stereo. This is the right choice for **first-person views, free cameras, and fly-throughs** — anywhere the camera moves through the scene.

### Use the built-in sample

1. Open **Window > Package Manager > Monado 3D Display**.
2. Expand the **Samples** section and click **Import** next to **Basic Scene**.
3. Open the imported scene at `Assets/Samples/Monado 3D Display/0.1.0/Basic Scene/BasicScene.unity`.
4. The Main Camera already has a **Monado3DCamera** component.

### Or build it from scratch

This takes about 5 minutes and teaches you the setup:

1. **File > New Scene > Basic (Built-in)**. Save it as `CameraDemo.unity`.

2. Select **Main Camera** in the hierarchy.

3. **Add Component > Monado3D > Camera-Centric Rig** (the `Monado3DCamera` component).

4. In the Inspector, you'll see:
   - A help box explaining camera-centric mode
   - **Connected Display** info (populated when runtime is active)
   - **Stereo Tunables**: IPD Factor, Parallax Factor
   - **Camera-Centric Parameters**: Convergence Distance (with diopters readout), FOV Override

5. Leave all defaults (IPD Factor = 1.0, Convergence = 0.5 m). The plugin auto-computes FOV from the display geometry.

6. Build some test content at varying depths (the stereo effect depends on depth variation):

   ```
   Hierarchy:
     Main Camera          [Monado3DCamera] at (0, 0, 0)
     NearCube             Red cube at (−0.3, 0, 0.3)    — pops out
     MidCube              Green cube at (0, 0, 0.5)      — at screen plane
     FarCube              Blue cube at (0.3, 0, 1.0)     — recedes behind screen
     Floor                Plane at (0, −0.3, 0.5)
     Directional Light
   ```

   To create each cube: **GameObject > 3D Object > Cube**, position it, scale to ~0.15–0.25, assign a colored material.

7. **Why this content suits camera-centric mode:** The camera is at the origin looking forward — like a person standing in a room. Objects are placed at varying distances to show the stereo depth gradient: near objects pop out of the screen, objects at the convergence distance sit on the screen plane, and far objects recede. Moving the camera (or the viewer's head) creates natural motion parallax through the scene.

### Key parameters to experiment with

| Parameter | Try this | Effect |
|-----------|----------|--------|
| Convergence Distance | 0.3 m | Screen plane moves closer — more content "pops out" |
| Convergence Distance | 1.0 m | Screen plane moves farther — most content is in front of screen |
| IPD Factor | 0.5 | Reduces stereo intensity (gentler depth) |
| IPD Factor | 2.0 | Exaggerates stereo (dramatic depth, may cause discomfort) |
| FOV Override | 60 | Locks FOV regardless of display geometry |

---

## Step 5: Demo Scene B — Display-Centric Mode

Display-centric mode anchors a virtual display in the scene. The viewer looks "into" it like a window or display case. This is the right choice for **tabletop views, product viewers, museum exhibits, and AR-like object displays**.

### Use the built-in sample

1. Open **Window > Package Manager > Monado 3D Display**.
2. Import the **Display Scene** sample.
3. Follow the README in the imported folder to set up the hierarchy.

### Or build it from scratch

1. **File > New Scene > Basic (Built-in)**. Save it as `DisplayDemo.unity`.

2. **Create the virtual display anchor:**
   - **GameObject > Create Empty**, name it **VirtualDisplay**.
   - Position it at `(0, 0, 0.8)` — roughly where a tabletop display would sit.

3. **Add Component > Monado3D > Display-Centric Rig** (`Monado3DDisplay` component).

4. **Parent the camera under the display:**
   - Drag **Main Camera** onto **VirtualDisplay** in the hierarchy to make it a child.
   - Set Main Camera's local position to `(0, 0.3, −0.5)` — slightly above and behind the display, looking at it.

5. **Build tabletop content** — objects arranged around the display's origin:

   ```
   Hierarchy:
     VirtualDisplay       [Monado3DDisplay] at (0, 0, 0.8)
       Main Camera        at local (0, 0.3, −0.5)
     Turntable            Empty at (0, 0, 0.8) — parent for rotating objects
       GoldSphere         Gold sphere at center (0, 0, 0)
       RedOrbit           Small red sphere at (0.25, 0.08, −0.1) — pops out
       TealPillar         Cylinder at (−0.2, 0.09, 0.15) — behind display plane
       PurpleCube         Cube at (0, −0.02, −0.22) — in front
     Platform             Flat cylinder at (0, −0.08, 0.8) — dark base
     Floor                Plane
     Key Light / Fill Light
   ```

   Or simply add `DisplaySceneSetup.cs` to the VirtualDisplay object — it creates all this programmatically and adds a slow turntable rotation.

6. **Why this content suits display-centric mode:** The display is a fixed anchor in the scene — like a glass case on a table. Objects at the display's origin sit "at the glass." Objects in front pop out toward the viewer; objects behind recede into the case. The turntable rotation is especially effective here because the viewer's eye tracking is relative to the display, not the camera — the 3D effect is stable regardless of where the viewer stands.

### Key parameters to experiment with

| Parameter | Try this | Effect |
|-----------|----------|--------|
| Depth Scale | 0.5 | Compresses depth behind the screen (less distortion at edges) |
| Virtual Display Scale | 2.0 | Virtual display is 2x physical size — scene objects appear smaller |
| Virtual Display Scale | 0.5 | Half-size — objects appear larger (magnifier effect) |
| Parallax Factor | 0.0 | Disables motion parallax — stereo still works via IPD |

---

## Step 6: Test in the Editor

1. Verify the runtime is configured: **Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display** should show either a connected display or the sim_display info.

2. Open one of the demo scenes and press **Play**.

3. **With sim_display (`SIM_DISPLAY_OUTPUT=sbs`):** You'll see a side-by-side stereo pair in the Game view. The left half is the left eye, right half is the right eye. Cross your eyes or use a stereo viewer to verify depth.

4. **With real hardware:** The runtime handles display output. The Game view may show a single eye or a composited view depending on the runtime configuration.

5. **Editor Preview Window** (works without Play Mode):
   - Open **Window > Monado3D > Preview Window**
   - Select **SideBySide** mode for a stereo pair preview
   - This works without the runtime — useful for checking scene layout

6. **Enable Log Eye Tracking** on either component to see per-frame eye positions in the Console:
   ```
   [Monado3D] Eyes: L=(0.032, 0.001, 0.504), R=(-0.031, 0.001, 0.504), tracked=True
   ```

---

## Step 7: Build a Standalone App

### Windows Build

1. **File > Build Settings**.
2. Select **Windows, Mac, Linux** as the platform. Click **Switch Platform** if needed.
3. Set:
   - **Target Platform:** Windows
   - **Architecture:** x86_64
4. Add your demo scene(s) to the **Scenes In Build** list (drag from Project window or click **Add Open Scenes**).
5. Click **Player Settings** and verify:
   - **XR Plug-in Management > Standalone**: OpenXR is checked
   - **OpenXR > Features**: Monado 3D Display is checked
6. Click **Build**. Choose an output folder (e.g., `Builds/Windows/`).
7. Unity produces:
   ```
   Builds/Windows/
     YourApp.exe                     ← main executable
     YourApp_Data/
       Plugins/x86_64/
         monado3d_unity.dll          ← native plugin (auto-included)
     UnityPlayer.dll
     ...
   ```

The output is a standard `.exe` — no installer is needed for testing. For distribution, you can zip the folder or use an installer tool (Inno Setup, NSIS, etc.) if desired, but for pipeline testing, running the `.exe` directly is sufficient.

### macOS Build

1. **File > Build Settings**.
2. Select **macOS** as the platform. Click **Switch Platform** if needed.
3. Add your demo scene(s) to **Scenes In Build**.
4. Verify XR settings (same as Windows above).
5. Click **Build**. Choose an output location.
6. Unity produces a `.app` bundle:
   ```
   YourApp.app/
     Contents/
       Plugins/
         libmonado3d_unity.dylib     ← native plugin (auto-included)
       MacOS/
         YourApp                     ← main executable
       ...
   ```

> **macOS Gatekeeper:** The first time you run an unsigned `.app`, macOS may block it. Right-click > Open, or: `xattr -cr YourApp.app` to strip the quarantine flag.

### Cross-Compiling (macOS to Windows)

You can build a Windows `.exe` from the macOS editor:

1. Install **Windows Build Support (Mono)** module via Unity Hub:
   - Unity Hub > Installs > (your version) > Add Modules > Windows Build Support (Mono)
2. In Build Settings, set **Target Platform** to **Windows**, architecture **x86_64**.
3. Build as normal. Unity includes the Windows DLL automatically.

The resulting `.exe` must be run on a Windows machine with the Monado runtime installed.

---

## Step 8: Run the Built App

### Running on Windows

**With sim_display (no hardware):**

Open Command Prompt in the build folder:
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_monado-dev.json
set SIM_DISPLAY_ENABLE=1
set SIM_DISPLAY_OUTPUT=sbs

YourApp.exe
```

**With the installed runtime:**

If the Monado runtime was installed system-wide (`SRMonadoInstaller.exe`), just double-click `YourApp.exe` — no environment variables needed.

**With runtime in a custom path:**
```cmd
set XR_RUNTIME_JSON=C:\path\to\openxr_monado-dev.json
YourApp.exe
```

### Running on macOS

Open Terminal:
```bash
export XR_RUNTIME_JSON=/path/to/SRMonado-macOS/share/openxr/1/openxr_monado.json

# For testing without hardware:
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT=sbs

# Launch the app
open YourApp.app

# Or launch directly (useful if 'open' doesn't pass env vars):
./YourApp.app/Contents/MacOS/YourApp
```

> **Important:** On macOS, `open` may not forward environment variables to the app. If the app doesn't connect to the runtime, use the direct executable path instead.

### Verifying the Pipeline

Once the app is running, verify each stage:

| Check | What to look for |
|-------|-----------------|
| **Runtime connects** | No "No OpenXR runtime found" error in the Player.log |
| **Stereo renders** | With `SIM_DISPLAY_OUTPUT=sbs`, you see a side-by-side stereo pair |
| **Depth is correct** | Near objects appear shifted between left/right eyes; far objects are nearly identical |
| **Camera-centric mode** | Scene parallax shifts as (simulated) head moves |
| **Display-centric mode** | Objects rotate smoothly on the turntable; depth is relative to the display anchor |
| **Log file location** | Windows: `%APPDATA%\..\LocalLow\<Company>\<Product>\Player.log` |
| | macOS: `~/Library/Logs/<Company>/<Product>/Player.log` |

### Reading the Log

Search the Player.log for these key lines:

```
[Monado3D] Feature enabled, hooking OpenXR instance
[Monado3D] Display info: 1920x1080, 27.0x15.2 cm, viewer at 500 mm
[Monado3D] Eye tracking active
```

If you see `[Monado3D] Feature not active` or no Monado3D lines at all, the feature is not enabled in XR settings — go back to Step 2.

---

## What to Look For

### Camera-centric demo
- **Stereo depth gradient:** The red near cube should appear to float in front of the screen. The green mid cube sits at the screen plane. The blue far cube is behind the screen.
- **Motion parallax:** When the viewer moves their head (or the sim_display keyboard controls move the simulated eye position), the scene shifts naturally — near objects move more than far objects.
- **Convergence plane:** Adjusting convergence distance moves where the "screen plane" sits in depth. At 0.3 m everything looks like it pops out; at 1.0 m most content recedes.

### Display-centric demo
- **Turntable stability:** As objects rotate, the 3D effect should remain stable — objects don't wobble or swim. This is because depth is computed relative to the display anchor, not the camera.
- **Pop-out vs. recede:** The red orbit sphere and purple cube are in front of the display plane (pop out); the teal pillar extends behind (recedes into the display).
- **Scale effect:** Changing Virtual Display Scale makes the whole scene appear larger or smaller, like zooming a magnifying glass.

---

## Next Steps

- **Tune the stereo parameters** — see the [Stereo Tunables Reference](../README.md#stereo-tunables-reference) for what each parameter does physically.
- **Add a 2D UI overlay** — see [2D UI Overlay](../README.md#2d-ui-overlay) for routing a Canvas to a compositor layer.
- **Preview without Play Mode** — see [Editor Preview](../README.md#editor-preview) for SBS and readback modes.
- **Deploy to end users** — see [Deploying to End Users](../README.md#deploying-to-end-users) for runtime installation on target machines.
- **Build for both platforms** — the plugin includes both Windows and macOS binaries. Unity selects the correct one per build target.
