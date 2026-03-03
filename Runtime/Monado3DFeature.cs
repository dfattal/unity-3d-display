// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.XR.OpenXR;
using UnityEngine.XR.OpenXR.Features;

#if UNITY_EDITOR
using UnityEditor;
using UnityEditor.XR.OpenXR.Features;
#endif

namespace Monado.Display3D
{
#if UNITY_EDITOR
    [OpenXRFeature(
        UiName = "Monado 3D Display",
        BuildTargetGroups = new[] {
            UnityEditor.BuildTargetGroup.Standalone
        },
        Company = "Leia Inc.",
        Desc = "Enables stereo rendering on 3D light field displays via Monado OpenXR runtime. " +
               "Provides Kooima asymmetric frustum projection, display-centric and camera-centric " +
               "stereo rig modes, and 2D UI overlay support.",
        DocumentationLink = "https://github.com/niceBrainChild/CNSDK-OpenXR",
        OpenxrExtensionStrings = ExtensionStrings,
        Version = "0.1.0",
        FeatureId = FeatureId
    )]
#endif
    public class Monado3DFeature : OpenXRFeature
    {
        public const string FeatureId = "com.monado.display3d.feature";

        // Extensions requested from the runtime
        private const string ExtensionStrings =
            "XR_EXT_display_info " +
            "XR_EXT_win32_window_binding " +
            "XR_EXT_macos_window_binding";

        /// <summary>Singleton instance, set during OnInstanceCreate.</summary>
        public static Monado3DFeature Instance { get; private set; }

        /// <summary>Cached display info from the runtime.</summary>
        public Monado3DDisplayInfo DisplayInfo { get; private set; }

        /// <summary>Whether eye tracking data is being received.</summary>
        public bool IsEyeTracked { get; private set; }

        /// <summary>Raw left eye position in display space (meters).</summary>
        public Vector3 LeftEyePosition { get; private set; }

        /// <summary>Raw right eye position in display space (meters).</summary>
        public Vector3 RightEyePosition { get; private set; }

        private bool m_HooksInstalled;

        // ====================================================================
        // OpenXR Feature lifecycle
        // ====================================================================

        /// <inheritdoc />
        protected override IntPtr HookGetInstanceProcAddr(IntPtr func)
        {
            // Install our native hook chain. The native plugin stores the next-in-chain
            // function pointer and returns our interceptor.
            IntPtr hooked = Monado3DNative.monado3d_install_hooks(func);
            m_HooksInstalled = (hooked != IntPtr.Zero);

            if (!m_HooksInstalled)
            {
                Debug.LogError("[Monado3D] Failed to install native OpenXR hooks");
                return func; // Pass through unmodified
            }

            Debug.Log("[Monado3D] Native OpenXR hooks installed");
            return hooked;
        }

        /// <inheritdoc />
        protected override bool OnInstanceCreate(ulong xrInstance)
        {
            Instance = this;
            Debug.Log("[Monado3D] OpenXR instance created");
            return true;
        }

        /// <inheritdoc />
        protected override void OnSystemChange(ulong xrSystem)
        {
            // Display info is extracted by our hooked xrGetSystemProperties.
            // Query it from the native plugin.
            RefreshDisplayInfo();
        }

        /// <inheritdoc />
        protected override void OnSessionCreate(ulong xrSession)
        {
            Debug.Log("[Monado3D] OpenXR session created");
        }

        /// <inheritdoc />
        protected override void OnSessionDestroy(ulong xrSession)
        {
            Debug.Log("[Monado3D] OpenXR session destroyed");
        }

        /// <inheritdoc />
        protected override void OnInstanceDestroy(ulong xrInstance)
        {
            Instance = null;
            m_HooksInstalled = false;
        }

        // ====================================================================
        // Public API
        // ====================================================================

        /// <summary>
        /// Set stereo rig tunables. Called each LateUpdate by Monado3DDisplay or Monado3DCamera.
        /// Thread-safe: double-buffered in the native plugin.
        /// </summary>
        public void SetTunables(Monado3DTunables tunables)
        {
            if (!m_HooksInstalled) return;

            Monado3DNative.monado3d_set_tunables(
                tunables.ipdFactor,
                tunables.parallaxFactor,
                tunables.perspectiveFactor,
                tunables.scaleFactor,
                tunables.convergenceDistance,
                tunables.fovOverride,
                tunables.cameraCentricMode ? 1 : 0);
        }

        /// <summary>
        /// Refresh display info from the native plugin cache.
        /// </summary>
        public void RefreshDisplayInfo()
        {
            if (!m_HooksInstalled) return;

            Monado3DNative.monado3d_get_display_info(
                out float wm, out float hm,
                out uint pw, out uint ph,
                out float nx, out float ny, out float nz,
                out float sx, out float sy,
                out int modeSwitch, out int valid);

            DisplayInfo = new Monado3DDisplayInfo
            {
                displayWidthMeters = wm,
                displayHeightMeters = hm,
                displayPixelWidth = pw,
                displayPixelHeight = ph,
                nominalViewerX = nx,
                nominalViewerY = ny,
                nominalViewerZ = nz,
                recommendedViewScaleX = sx,
                recommendedViewScaleY = sy,
                supportsDisplayModeSwitch = modeSwitch != 0,
                isValid = valid != 0,
            };

            if (DisplayInfo.isValid)
            {
                Debug.Log($"[Monado3D] Display: {pw}x{ph}px, " +
                          $"{wm * 100:F1}x{hm * 100:F1}cm, " +
                          $"nominal=({nx * 1000:F0},{ny * 1000:F0},{nz * 1000:F0})mm, " +
                          $"scale={sx:F2}x{sy:F2}");
            }
        }

        /// <summary>
        /// Refresh eye position data from native plugin. Call each frame if needed.
        /// </summary>
        public void RefreshEyePositions()
        {
            if (!m_HooksInstalled) return;

            Monado3DNative.monado3d_get_eye_positions(
                out float lx, out float ly, out float lz,
                out float rx, out float ry, out float rz,
                out int tracked);

            LeftEyePosition = new Vector3(lx, ly, lz);
            RightEyePosition = new Vector3(rx, ry, rz);
            IsEyeTracked = tracked != 0;
        }

        /// <summary>
        /// Set the scene transform applied to raw eye positions before Kooima computation.
        /// This is the "parent camera pose" from the Unity scene hierarchy.
        /// Chain: raw eyes -> scene transform -> tunables -> Kooima.
        ///
        /// Use this to map raw DISPLAY-space eye positions into your scene's coordinate
        /// frame, mirroring the test app's player transform (rotation, zoom, position offset).
        /// </summary>
        /// <param name="position">Translation offset in display space (meters).</param>
        /// <param name="orientation">Rotation quaternion applied to eye positions.</param>
        /// <param name="zoomScale">Zoom scale: eye positions divided by this before Kooima. 1.0 = no zoom.</param>
        /// <param name="enabled">Whether to apply the transform.</param>
        public void SetSceneTransform(Vector3 position, Quaternion orientation, float zoomScale = 1.0f, bool enabled = true)
        {
            if (!m_HooksInstalled) return;

            Monado3DNative.monado3d_set_scene_transform(
                position.x, position.y, position.z,
                orientation.x, orientation.y, orientation.z, orientation.w,
                zoomScale,
                enabled ? 1 : 0);
        }

        /// <summary>
        /// Set the window handle for runtime rendering. Must be called before session creation.
        /// On Windows: pass the HWND. On macOS: pass the NSView pointer.
        /// </summary>
        public void SetWindowHandle(IntPtr handle)
        {
            if (!m_HooksInstalled) return;
            Monado3DNative.monado3d_set_window_handle(handle);
        }

        /// <summary>
        /// Request 2D or 3D display mode via XR_EXT_display_info.
        /// </summary>
        /// <param name="mode3d">true for 3D, false for 2D.</param>
        /// <returns>true if the request succeeded.</returns>
        public bool RequestDisplayMode(bool mode3d)
        {
            if (!m_HooksInstalled) return false;
            return Monado3DNative.monado3d_request_display_mode(mode3d ? 1 : 0) != 0;
        }
    }
}
