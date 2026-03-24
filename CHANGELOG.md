# Changelog

All notable changes to the DisplayXR Unity plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
