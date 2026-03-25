# Changelog

All notable changes to the DisplayXR Unity plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.6] - 2026-03-25

### Fixed
- Fix preview window not opening from menu (#40): null-guard custom editors to prevent `SerializedObjectNotCreatableException` during domain reload
- D3D11 TYPELESS swapchain textures: replace proxy texture copy with thin COM wrapper that overrides `GetDesc()` to report concrete format — zero-copy, no extra textures (#36)

## [0.3.5] - 2026-03-25

### Changed
- Cross-device atlas blit via DXGI shared bridge texture (#38)
  - Unity renders atlas on its D3D12 device, then `Graphics.CopyTexture` to a bridge texture shared on both devices
  - `CopyTextureRegion` from bridge to swapchain on the runtime's device
  - Completes the cross-device rendering pipeline started in 0.3.4

## [0.3.4] - 2026-03-25

### Changed
- D3D12: use separate device for runtime session, shared texture via DXGI handle for Unity (#35)
  - Sharing Unity's D3D12 device with the runtime caused device removal
  - Create dedicated D3D12 device for the runtime OpenXR session
  - Use `OpenSharedHandle` on Unity's device for `CreateExternalTexture`
  - Atlas blit skipped (cross-device TODO)

## [0.3.3] - 2026-03-25

### Fixed
- D3D12 atlas blit: remove explicit resource barriers, rely on implicit COMMON state promotion for cross-queue copy operations (#35)

## [0.3.2] - 2026-03-25

### Fixed
- D3D12 OpenXR struct type IDs: corrected from `1000027xxx` to `1000028xxx` (`XR_TYPE_GRAPHICS_BINDING_D3D12_KHR`, `XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR`, `XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR`) (#35)

## [0.3.1] - 2026-03-25

### Fixed
- Crash in `set_unity_device` on Windows D3D12: validate `ID3D12Resource` via `QueryInterface` before calling `GetDevice`, preventing access violation when Unity passes a non-D3D12 resource (#35)

## [0.3.0] - 2026-03-25

### Changed
- Migrate Windows standalone preview from D3D11 to D3D12 (#35)
  - Replace D3D11 device/context with D3D12 device/queue/command list/fence
  - D3D12 shared texture via `CreateCommittedResource` + `D3D12_HEAP_FLAG_SHARED`
  - Atlas blit with D3D12 command list, resource barriers, and fence sync
  - `XrGraphicsBindingD3D12KHR` for session creation
  - Platform-conditional Y-flip (Metal vs D3D12)
  - Supports both D3D11 and D3D12 Unity graphics backends

## [0.2.2] - 2026-03-24

### Fixed
- CS0104 ambiguous `Object` reference in `DisplayXRPreviewSession.cs` — qualify as `UnityEngine.Object` to resolve conflict with `System.Object` (#35)

## [0.2.1] - 2026-03-24

### Fixed
- Null texture in `set_unity_device`: force `RenderTexture.Create()` before `GetNativeTexturePtr()` to ensure GPU resource is allocated (#35)

## [0.2.0] - 2026-03-25

### Fixed
- Windows standalone preview: use Unity's own D3D11 device instead of creating a separate one, fixing cross-device TDR crashes when sharing textures between devices (#35)

## [0.1.9] - 2026-03-25

### Fixed
- Revert shared texture to B8G8R8A8_UNORM — runtime rejects TYPELESS format (`xrCreateSession` fails with -6). The C# `linear=true` flag is sufficient for correct gamma handling.

## [0.1.8] - 2026-03-24

### Fixed
- D3D11 shared texture compatibility: use TYPELESS format with linear SRV to avoid gamma/format mismatch issues in the standalone preview rendering pipeline

## [0.1.7] - 2026-03-24

### Added
- Windows standalone preview: D3D11 swapchain image acquisition, atlas blit from shared texture, and `xrEndFrame` submission — completes the Windows standalone preview rendering pipeline

## [0.1.6] - 2026-03-24

### Fixed
- Windows crash: `displayxr_standalone_get_shared_texture` now returns `ID3D11Texture2D*` (what Unity's `CreateExternalTexture` expects) instead of the DXGI shared `HANDLE` (which is for cross-device sharing with the runtime)

## [0.1.5] - 2026-03-24

### Added
- Windows standalone preview: D3D11 shared texture creation and DXGI handle passing to runtime via Win32 window binding — enables zero-copy GPU texture sharing for preview output

## [0.1.4] - 2026-03-23

### Fixed
- Windows standalone preview: create D3D11 device with correct adapter LUID and pass `XrGraphicsBindingD3D11KHR` to session creation (fixes `xrCreateSession` error -38)

## [0.1.3] - 2026-03-23

### Added
- Windows standalone preview: implement `LoadLibrary`/`GetProcAddress` runtime loading and Win32 window binding for session creation — standalone preview now starts on Windows

## [0.1.2] - 2026-03-23

### Fixed
- Windows DLL plugin settings: enable Editor platform so standalone preview and Play Mode can load `displayxr_unity.dll` in the Windows editor

## [0.1.1] - 2026-03-23

### Fixed
- Standalone preview now discovers the runtime via Windows registry (`Khronos\OpenXR\1\ActiveRuntime`) when `XR_RUNTIME_JSON` is not set
- Settings page shows runtime discovery source (env var vs registry)
- UPM git URL install: added `.gitattributes` to prevent binary corruption, documented Git prerequisites for Windows and macOS
- Quick-start guide updated with git URL as primary install method

## [0.1.0] - 2026-03-23

### Added
- Initial release as standalone UPM package (moved from `openxr-3d-display` runtime repo)
- OpenXR Feature lifecycle (`DisplayXRFeature`) with native hook chain
- Camera-centric stereo rig (`DisplayXRCamera`) for retrofitting existing scenes
- Display-centric stereo rig (`DisplayXRDisplay`) for virtual display placement
- Kooima asymmetric frustum projection via native plugin (display3d + camera3d libraries)
- Eye tracking integration through OpenXR extensions
- Stereo tunables: IPD factor, parallax factor, perspective factor, inverse convergence distance
- 2D UI overlay component (`DisplayXRWindowSpaceUI`) for HUDs and menus
- Standalone editor preview window with camera selector, rendering mode controls, and zero-copy GPU texture sharing (IOSurface/DXGI)
- Game View overlay (`DisplayXRGameViewOverlay`) for Play Mode shared texture output
- Canvas-aware shared texture cropping via `xrSetSharedTextureOutputRectEXT`
- Custom inspectors for camera-centric and display-centric modes
- Project Settings page showing runtime status and display info
- Native plugin source (`native~/`) with independent CMake build
- CI workflow for Windows (MSVC) and macOS (Universal) native builds
- Cross-platform support: Windows x64 and macOS
- Cross-compilation support: build Windows target from macOS editor
- `.gitattributes` for binary file protection
- Quick start guide and comprehensive README with troubleshooting
