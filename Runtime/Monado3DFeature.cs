// Copyright 2024-2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.XR.Management;
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
        Company = "Monado 3D Display",
        Desc = "Enables stereo rendering on 3D light field displays via Monado OpenXR runtime. " +
               "Provides Kooima asymmetric frustum projection, display-centric and camera-centric " +
               "stereo rig modes, and 2D UI overlay support.",
        DocumentationLink = "https://github.com/dfattal/unity-3d-display",
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
            "XR_EXT_cocoa_window_binding";

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

        /// <summary>Whether a shared GPU texture is available for zero-copy preview.</summary>
        public bool SharedTextureAvailable { get; private set; }

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

            // Force multi-pass rendering. Single-pass instanced is broken on
            // macOS/Metal (confirmed Unity bug, Won't Fix) and incompatible
            // with our per-eye asymmetric Kooima frustum projection.
            var settings = OpenXRSettings.Instance;
            if (settings != null && settings.renderMode != OpenXRSettings.RenderMode.MultiPass)
            {
                Debug.Log("[Monado3D] Forcing MultiPass render mode (required for asymmetric frustum projection)");
                settings.renderMode = OpenXRSettings.RenderMode.MultiPass;

                // Also update the serialized backing field so ApplySettings()
                // (which runs after OnInstanceCreate) doesn't overwrite us.
                var field = typeof(OpenXRSettings).GetField("m_renderMode",
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
                if (field != null)
                    field.SetValue(settings, OpenXRSettings.RenderMode.MultiPass);
            }

            Debug.Log("[Monado3D] OpenXR instance created");
            return true;
        }

        /// <inheritdoc />
        protected override void OnSystemChange(ulong xrSystem)
        {
            // Display info is extracted by our hooked xrGetSystemProperties.
            // Query it from the native plugin.
            RefreshDisplayInfo();

            // Create shared GPU texture now — before xrCreateSession, which
            // injects the IOSurface pointer into the window binding struct.
            if (DisplayInfo.isValid && DisplayInfo.displayPixelWidth > 0 && DisplayInfo.displayPixelHeight > 0)
            {
                IntPtr ptr = Monado3DNative.monado3d_create_shared_texture(
                    DisplayInfo.displayPixelWidth, DisplayInfo.displayPixelHeight);
                SharedTextureAvailable = (ptr != IntPtr.Zero);

                if (SharedTextureAvailable)
                    Debug.Log($"[Monado3D] Shared texture created: {DisplayInfo.displayPixelWidth}x{DisplayInfo.displayPixelHeight}");
                else
                    Debug.Log("[Monado3D] Shared texture not available, using CPU readback");
            }
        }

        /// <inheritdoc />
        protected override void OnSessionCreate(ulong xrSession)
        {
            Debug.Log("[Monado3D] OpenXR session created");

            // Fallback: Unity's OnSystemChange is unreliable in some versions.
            // By session creation time, xrGetSystemProperties has definitely run
            // and our native hook has cached the display info + created the IOSurface.
            if (!DisplayInfo.isValid)
            {
                Debug.Log("[Monado3D] OnSystemChange was not called — refreshing display info now");
                RefreshDisplayInfo();
            }

            // Check if the native layer already created the shared texture
            if (!SharedTextureAvailable && DisplayInfo.isValid)
            {
                Monado3DNative.monado3d_get_shared_texture(
                    out IntPtr nativePtr, out uint w, out uint h, out int ready);
                SharedTextureAvailable = (ready != 0 && nativePtr != IntPtr.Zero);

                if (SharedTextureAvailable)
                    Debug.Log($"[Monado3D] Shared texture available: {w}x{h}");
                else
                    Debug.Log("[Monado3D] Shared texture not available, using CPU readback");
            }
        }

        /// <inheritdoc />
        protected override void OnSessionDestroy(ulong xrSession)
        {
            Debug.Log("[Monado3D] OnSessionDestroy BEGIN");

            if (SharedTextureAvailable)
            {
                Debug.Log("[Monado3D] OnSessionDestroy: destroying shared texture");
                try { Monado3DNative.monado3d_destroy_shared_texture(); }
                catch (System.Exception e) { Debug.LogWarning($"[Monado3D] Shared texture cleanup: {e.Message}"); }
                SharedTextureAvailable = false;
            }

            // Prevent Unity's post-destroy xrPollEvent crash.
            // Unity's Deinitialize() calls ProcessOpenXRMessageLoop after
            // Internal_DestroySession, through dispatch trampolines that
            // reference freed runtime memory. Throttle the pump so it
            // returns early without calling Internal_PumpMessageLoop.
            UnhookMessageLoop();

            Debug.Log("[Monado3D] OnSessionDestroy END");
        }

        /// <inheritdoc />
        protected override void OnInstanceDestroy(ulong xrInstance)
        {
            Debug.Log("[Monado3D] OnInstanceDestroy BEGIN");
            Instance = null;
            m_HooksInstalled = false;
            // Re-throttle in case the state changed between OnSessionDestroy and here
            UnhookMessageLoop();
            Debug.Log("[Monado3D] OnInstanceDestroy END");
        }

        /// <summary>
        /// Prevent post-destroy xrPollEvent calls that crash through freed dispatch trampolines.
        /// Three-layer defense:
        /// 1. Remove ProcessOpenXRMessageLoop from onBeforeRender (multiple times for domain reload duplicates)
        /// 2. Force currentOpenXRState to XrExiting so the method enters its throttle path
        /// 3. Set lastPollCheckTime to MaxValue so the throttle always returns early
        /// This makes ProcessOpenXRMessageLoop a complete no-op even if still called
        /// (e.g., from editor repaint path or explicit calls in Deinitialize).
        /// </summary>
        private static void UnhookMessageLoop()
        {
            try
            {
                var generalSettings = XRGeneralSettings.Instance;
                if (generalSettings == null || generalSettings.Manager == null) return;
                var loader = generalSettings.Manager.activeLoader as OpenXRLoaderBase;
                if (loader == null) return;

                var bindingFlags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;

                // Layer 1: Remove delegate from onBeforeRender.
                // Remove multiple times to handle duplicate registrations from domain reload
                // (Unity re-registers at lines 780-781 of OpenXRLoader.cs during domain reload).
                var method = typeof(OpenXRLoaderBase).GetMethod("ProcessOpenXRMessageLoop", bindingFlags);
                if (method != null)
                {
                    var callback = (UnityEngine.Events.UnityAction)
                        System.Delegate.CreateDelegate(typeof(UnityEngine.Events.UnityAction), loader, method);
                    for (int i = 0; i < 5; i++)
                        Application.onBeforeRender -= callback;
                    Debug.Log("[Monado3D] Removed ProcessOpenXRMessageLoop from onBeforeRender");
                }

                // Layer 2: Force currentOpenXRState to XrExiting.
                // ProcessOpenXRMessageLoop checks this state — XrExiting enters the throttle path
                // instead of calling Internal_PumpMessageLoop immediately.
                var stateField = typeof(OpenXRLoaderBase).GetField("currentOpenXRState", bindingFlags);
                if (stateField != null)
                {
                    var nativeEventType = typeof(OpenXRFeature).GetNestedType("NativeEvent",
                        System.Reflection.BindingFlags.NonPublic);
                    if (nativeEventType != null)
                    {
                        var xrExiting = System.Enum.Parse(nativeEventType, "XrExiting");
                        stateField.SetValue(loader, xrExiting);
                        Debug.Log("[Monado3D] Set currentOpenXRState to XrExiting");
                    }
                }

                // Layer 3: Set lastPollCheckTime to MaxValue.
                // The throttle condition is: if (time - lastPollCheckTime < k_IdlePollingWaitTimeInSeconds) return;
                // With MaxValue, (time - MaxValue) is hugely negative, always < threshold → always returns.
                var pollField = typeof(OpenXRLoaderBase).GetField("lastPollCheckTime", bindingFlags);
                if (pollField != null)
                {
                    pollField.SetValue(loader, double.MaxValue);
                    Debug.Log("[Monado3D] Set lastPollCheckTime to MaxValue");
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"[Monado3D] Failed to unhook message loop: {e.Message}");
            }
        }

        // ====================================================================
        // Validation
        // ====================================================================

#if UNITY_EDITOR
        /// <inheritdoc />
        protected override void GetValidationChecks(List<ValidationRule> rules, BuildTargetGroup targetGroup)
        {
            rules.Add(new ValidationRule(this)
            {
                message = "Monado 3D Display requires Multi-Pass render mode. " +
                          "Single-pass instanced is broken on macOS and incompatible with asymmetric frustum projection.",
                error = true,
                checkPredicate = () =>
                {
                    var settings = OpenXRSettings.GetSettingsForBuildTargetGroup(targetGroup);
                    return settings != null && settings.renderMode == OpenXRSettings.RenderMode.MultiPass;
                },
                fixIt = () =>
                {
                    var settings = OpenXRSettings.GetSettingsForBuildTargetGroup(targetGroup);
                    if (settings != null)
                        settings.renderMode = OpenXRSettings.RenderMode.MultiPass;
                },
                fixItAutomatic = true,
            });
        }
#endif

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

            // For camera-centric mode, fovOverride is already half_tan_vfov
            // (C# computes Mathf.Tan(fov_rad * 0.5f) before setting it).
            // For display-centric mode, fovOverride is unused (0).
            Monado3DNative.monado3d_set_tunables(
                tunables.ipdFactor,
                tunables.parallaxFactor,
                tunables.perspectiveFactor,
                tunables.virtualDisplayHeight,
                tunables.invConvergenceDistance,
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

        // Reusable buffers for stereo matrix retrieval
        private float[] m_LeftView = new float[16];
        private float[] m_LeftProj = new float[16];
        private float[] m_RightView = new float[16];
        private float[] m_RightProj = new float[16];

        /// <summary>
        /// Get the Kooima stereo matrices computed by the native library.
        /// Returns true if valid matrices are available.
        /// Matrices are column-major, OpenXR/OpenGL convention (right-hand, -Z forward,
        /// Z clip range [-1,1]). Caller must convert to Unity convention.
        /// </summary>
        public bool GetStereoMatrices(out Matrix4x4 leftView, out Matrix4x4 leftProj,
                                       out Matrix4x4 rightView, out Matrix4x4 rightProj)
        {
            leftView = leftProj = rightView = rightProj = Matrix4x4.identity;
            if (!m_HooksInstalled) return false;

            Monado3DNative.monado3d_get_stereo_matrices(
                m_LeftView, m_LeftProj, m_RightView, m_RightProj, out int valid);

            if (valid == 0) return false;

            leftView = FloatsToMatrix(m_LeftView);
            leftProj = FloatsToMatrix(m_LeftProj);
            rightView = FloatsToMatrix(m_RightView);
            rightProj = FloatsToMatrix(m_RightProj);
            return true;
        }

        private static Matrix4x4 FloatsToMatrix(float[] m)
        {
            // Column-major float[16] → Unity Matrix4x4 (also column-major)
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }

        /// <summary>
        /// Set the scene transform applied to raw eye positions before Kooima computation.
        /// This is the "parent camera pose" from the Unity scene hierarchy.
        /// Chain: raw eyes -> scene transform -> tunables -> Kooima.
        ///
        /// Use this to map raw LOCAL-space eye positions into your scene's coordinate
        /// frame, mirroring the test app's player transform (rotation, zoom, position offset).
        /// </summary>
        /// <param name="position">Translation offset in display space (meters).</param>
        /// <param name="orientation">Rotation quaternion applied to eye positions.</param>
        /// <param name="scale">Transform scale: spatial coordinates divided by this. (1,1,1) = no scaling.</param>
        /// <param name="enabled">Whether to apply the transform.</param>
        public void SetSceneTransform(Vector3 position, Quaternion orientation, Vector3 scale, bool enabled = true)
        {
            if (!m_HooksInstalled) return;

            Monado3DNative.monado3d_set_scene_transform(
                position.x, position.y, position.z,
                orientation.x, orientation.y, orientation.z, orientation.w,
                scale.x, scale.y, scale.z,
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
