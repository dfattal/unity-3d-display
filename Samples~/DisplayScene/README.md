# Display Scene Sample

Tabletop display-centric demo for the Monado 3D Display plugin. Objects rotate on a turntable, showcasing how display-centric mode anchors a virtual display in the scene — ideal for object-focused viewing.

## What's Included

- `DisplaySceneSetup.cs` — Script that creates a turntable with objects at varying depths (gold sphere at display plane, red sphere popping out, teal pillar behind, purple cube to the side)

## Quick Start

1. Import this sample via **Package Manager > Monado 3D Display > Samples > Display Scene**
2. Create a new scene (**File > New Scene > Basic (Built-in)**)
3. Create an empty GameObject named **VirtualDisplay** at `(0, 0, 0.8)`
4. Add the **Monado3DDisplay** component to VirtualDisplay (**Add Component > Monado3D > Display-Centric Rig**)
5. Add the **DisplaySceneSetup** component to VirtualDisplay
6. Parent the **Main Camera** under VirtualDisplay, position it at `(0, 0.3, -0.5)` looking forward
7. Set `XR_RUNTIME_JSON` environment variable to your Monado runtime
8. Enter Play Mode

## Why Display-Centric?

This scene places a virtual display as a physical anchor in the scene. The turntable rotates objects at varying depths relative to the display plane — some objects pop out toward the viewer, others recede behind the glass. This is the natural model for:

- Tabletop displays (looking down at a 3D diorama)
- Museum exhibits (a display case with depth)
- Product viewers (rotate a 3D object on a pedestal)
- Digital signage (a display mounted on a wall)

## Without Hardware

```
SIM_DISPLAY_ENABLE=1
SIM_DISPLAY_OUTPUT=sbs
```
