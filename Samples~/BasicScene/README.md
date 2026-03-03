# Basic Scene Sample

Minimal camera-centric stereo setup for testing the Monado 3D Display plugin.

## What's Included

- `BasicScene.unity` — A scene with a Main Camera using `Monado3DCamera` component, plus a few colored cubes at varying depths to verify stereo rendering
- `BasicSceneSetup.cs` — Script that creates the scene objects programmatically (useful if the .unity scene file doesn't load cleanly across Unity versions)

## Quick Start

1. Import this sample via Package Manager > Monado 3D Display > Samples > Basic Scene
2. Open `BasicScene.unity`
3. Verify the Main Camera has a `Monado3DCamera` component
4. Set `XR_RUNTIME_JSON` environment variable to your Monado runtime
5. Enter Play Mode

## Without Hardware

Set these environment variables before launching Unity:
```
SIM_DISPLAY_ENABLE=1
SIM_DISPLAY_OUTPUT=sbs
```
