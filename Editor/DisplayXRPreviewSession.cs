// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.IO;
using UnityEditor;
using UnityEngine;


namespace DisplayXR.Editor
{
    /// <summary>
    /// Manages a standalone OpenXR session for the editor preview window.
    /// Completely independent of Unity's XR subsystem — no play mode required.
    /// Lifecycle is tied to the preview window, not play/stop transitions.
    ///
    /// Frame loop (each EditorApplication.update tick):
    ///   poll_events → begin_frame → compute_stereo_views →
    ///   [render left/right cameras] → submit_frame
    /// </summary>
    [InitializeOnLoad]
    public static class DisplayXRPreviewSession
    {
        public static bool IsRunning => DisplayXRNative.displayxr_standalone_is_running() != 0;

        public static DisplayXRDisplayInfo DisplayInfo { get; private set; }
        public static bool IsEyeTracked { get; private set; }
        public static Vector3 LeftEyePosition { get; private set; }
        public static Vector3 RightEyePosition { get; private set; }
        public static bool SharedTextureAvailable { get; private set; }

        private static bool s_Polling;
        private static bool s_Stopping; // Guard against re-entrant ticks during teardown
        private static int s_FrameCount;

        // Rendering rig
        private static RenderTexture s_LeftRT;
        private static RenderTexture s_RightRT;
        private static Camera s_LeftCam;
        private static Camera s_RightCam;
        private static GameObject s_RigRoot;
        private static readonly float[] s_LeftView = new float[16];
        private static readonly float[] s_LeftProj = new float[16];
        private static readonly float[] s_RightView = new float[16];
        private static readonly float[] s_RightProj = new float[16];
        private static float s_NearZ = 0.3f;
        private static float s_FarZ = 1000f;

        static DisplayXRPreviewSession()
        {
            // Clean up standalone session on domain reload (script recompilation)
            AssemblyReloadEvents.beforeAssemblyReload += OnBeforeAssemblyReload;
            EditorApplication.quitting += OnEditorQuitting;
        }

        /// <summary>
        /// Start the standalone preview session.
        /// Loads the DisplayXR runtime directly and creates an OpenXR session.
        /// </summary>
        public static bool Start()
        {
            if (IsRunning)
            {
                Debug.Log("[DisplayXR-SA] Already running");
                return true;
            }

            string runtimeJson = FindRuntimeJson();
            if (string.IsNullOrEmpty(runtimeJson))
            {
                Debug.LogError("[DisplayXR-SA] Cannot find DisplayXR runtime. " +
                    "Set XR_RUNTIME_JSON environment variable or install the runtime.");
                return false;
            }

            Debug.Log($"[DisplayXR-SA] Starting with runtime: {runtimeJson}");
            int result = DisplayXRNative.displayxr_standalone_start(runtimeJson);

            if (result != 0)
            {
                Debug.Log("[DisplayXR-SA] Standalone session started");
                StartPolling();
                RefreshDisplayInfo();
                CreateRenderRig();
                return true;
            }

            Debug.LogError("[DisplayXR-SA] Failed to start standalone session");
            return false;
        }

        /// <summary>
        /// Stop the standalone preview session and release all resources.
        /// </summary>
        public static void Stop()
        {
            if (s_Stopping) return; // Prevent re-entrant teardown
            s_Stopping = true;

            StopPolling();
            DestroyRenderRig();

            try
            {
                DisplayXRNative.displayxr_standalone_stop();
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[DisplayXR-SA] Exception during stop: {e.Message}");
            }

            SharedTextureAvailable = false;
            DisplayInfo = default;
            s_Stopping = false;
            Debug.Log("[DisplayXR-SA] Standalone session stopped");
        }

        private static void StartPolling()
        {
            if (s_Polling) return;
            s_Polling = true;
            EditorApplication.update += FrameTick;
        }

        private static void StopPolling()
        {
            if (!s_Polling) return;
            s_Polling = false;
            EditorApplication.update -= FrameTick;
        }

        private static void FrameTick()
        {
            if (s_Stopping || !IsRunning)
            {
                StopPolling();
                return;
            }

            // 1. Poll OpenXR events (session state changes)
            try
            {
                DisplayXRNative.displayxr_standalone_poll_events();
            }
            catch (Exception e)
            {
                Debug.LogError($"[DisplayXR-SA] poll_events crashed: {e.Message}. Stopping session.");
                Stop();
                return;
            }

            // 2. Begin frame (xrWaitFrame + xrBeginFrame)
            int ok = DisplayXRNative.displayxr_standalone_begin_frame(out int shouldRender);
            if (ok == 0)
            {
                if (s_FrameCount++ % 300 == 0)
                    Debug.Log($"[DisplayXR-SA] FrameTick: begin_frame returned 0 (not ready)");
                return;
            }

            if (shouldRender == 0)
            {
                if (s_FrameCount++ % 300 == 0)
                    Debug.Log($"[DisplayXR-SA] FrameTick: shouldRender=0, submitting empty frame");
                DisplayXRNative.displayxr_standalone_end_frame_empty();
                return;
            }

            // 3. Push tunables + display pose from scene rig component
            PushRigParameters();

            // 4. Compute Kooima stereo view/projection matrices from native
            DisplayXRNative.displayxr_standalone_compute_stereo_views(
                s_NearZ, s_FarZ,
                s_LeftView, s_LeftProj, s_RightView, s_RightProj,
                out int matricesValid);

            if (matricesValid != 0 && s_LeftCam != null && s_RightCam != null)
            {
                if (s_FrameCount++ % 300 == 0)
                {
                    // Log raw eye positions from runtime
                    DisplayXRNative.displayxr_standalone_get_eye_positions(
                        out float elx, out float ely, out float elz,
                        out float erx, out float ery, out float erz,
                        out int trk);

                    // Log the actual camera world position from view matrix inverse
                    Matrix4x4 v = ColumnMajorToMatrix4x4(s_LeftView);
                    Matrix4x4 vi = v.inverse;
                    var camPos = new Vector3(vi.m03, vi.m13, vi.m23);

                    // Extract FOV angles from GL projection matrix:
                    // P[0]=2n/(r-l), P[5]=2n/(t-b), P[8]=(r+l)/(r-l), P[9]=(t+b)/(t-b)
                    // tan(left)  = (-1 + P[8]) / P[0]  =>  angle = atan
                    // tan(right) = ( 1 + P[8]) / P[0]
                    // tan(down)  = (-1 + P[9]) / P[5]
                    // tan(up)    = ( 1 + P[9]) / P[5]
                    float tanL = (-1f + s_LeftProj[8]) / s_LeftProj[0];
                    float tanR = ( 1f + s_LeftProj[8]) / s_LeftProj[0];
                    float tanD = (-1f + s_LeftProj[9]) / s_LeftProj[5];
                    float tanU = ( 1f + s_LeftProj[9]) / s_LeftProj[5];
                    float fovH = (Mathf.Atan(tanR) - Mathf.Atan(tanL)) * Mathf.Rad2Deg;
                    float fovV = (Mathf.Atan(tanU) - Mathf.Atan(tanD)) * Mathf.Rad2Deg;

                    Debug.Log($"[DisplayXR-SA] Frame {s_FrameCount}: " +
                        $"rawEyeL=({elx:F3},{ely:F3},{elz:F3}) " +
                        $"rawEyeR=({erx:F3},{ery:F3},{erz:F3}) tracked={trk}\n" +
                        $"  camWorldPos=({camPos.x:F3},{camPos.y:F3},{camPos.z:F3}) " +
                        $"near={s_NearZ:F3} far={s_FarZ:F1}\n" +
                        $"  L_FOV: L={Mathf.Atan(tanL)*Mathf.Rad2Deg:F1}° R={Mathf.Atan(tanR)*Mathf.Rad2Deg:F1}° " +
                        $"D={Mathf.Atan(tanD)*Mathf.Rad2Deg:F1}° U={Mathf.Atan(tanU)*Mathf.Rad2Deg:F1}° " +
                        $"(H={fovH:F1}° V={fovV:F1}°)");
                }

                // 4. Render both eyes
                RenderEye(s_LeftCam, s_LeftRT, s_LeftView, s_LeftProj);
                RenderEye(s_RightCam, s_RightRT, s_RightView, s_RightProj);

                // 5. Submit to OpenXR swapchain
                IntPtr leftNative = s_LeftRT.GetNativeTexturePtr();
                IntPtr rightNative = s_RightRT.GetNativeTexturePtr();
                DisplayXRNative.displayxr_standalone_submit_frame(leftNative, rightNative);
            }
            else
            {
                if (s_FrameCount++ % 300 == 0)
                    Debug.Log($"[DisplayXR-SA] Frame {s_FrameCount}: matrices invalid={matricesValid}, " +
                        $"leftCam={s_LeftCam != null}, rightCam={s_RightCam != null}");
                // No valid matrices — submit empty frame
                DisplayXRNative.displayxr_standalone_end_frame_empty();
            }

            // Refresh cached state for UI
            RefreshEyePositions();
            RefreshSharedTexture();
        }

        // ================================================================
        // Render rig: two hidden cameras rendering to RenderTextures
        // ================================================================

        private static void CreateRenderRig()
        {
            // Get swapchain size from runtime (per-eye resolution)
            DisplayXRNative.displayxr_standalone_get_swapchain_size(out uint sw, out uint sh);
            if (sw == 0 || sh == 0)
            {
                // Fallback to display pixel resolution
                sw = DisplayInfo.displayPixelWidth;
                sh = DisplayInfo.displayPixelHeight;
            }
            if (sw == 0 || sh == 0)
            {
                sw = 1920;
                sh = 1080;
            }

            Debug.Log($"[DisplayXR-SA] Creating render rig: {sw}x{sh} per eye");

            // Create RenderTextures matching swapchain resolution
            s_LeftRT = new RenderTexture((int)sw, (int)sh, 24, RenderTextureFormat.ARGB32);
            s_LeftRT.name = "DisplayXR_SA_Left";
            s_LeftRT.Create();

            s_RightRT = new RenderTexture((int)sw, (int)sh, 24, RenderTextureFormat.ARGB32);
            s_RightRT.name = "DisplayXR_SA_Right";
            s_RightRT.Create();

            // Create hidden camera rig (HideFlags prevent it showing in hierarchy)
            s_RigRoot = new GameObject("DisplayXR_SA_Rig");
            s_RigRoot.hideFlags = HideFlags.HideAndDontSave;

            var leftGo = new GameObject("LeftEye");
            leftGo.hideFlags = HideFlags.HideAndDontSave;
            leftGo.transform.SetParent(s_RigRoot.transform);
            s_LeftCam = leftGo.AddComponent<Camera>();
            s_LeftCam.enabled = false; // We render manually via Camera.Render()
            s_LeftCam.targetTexture = s_LeftRT;

            var rightGo = new GameObject("RightEye");
            rightGo.hideFlags = HideFlags.HideAndDontSave;
            rightGo.transform.SetParent(s_RigRoot.transform);
            s_RightCam = rightGo.AddComponent<Camera>();
            s_RightCam.enabled = false;
            s_RightCam.targetTexture = s_RightRT;

            // Copy settings from scene camera if available
            Camera sceneCam = Camera.main;
            if (sceneCam == null)
            {
                var allCams = Camera.allCameras;
                if (allCams.Length > 0) sceneCam = allCams[0];
            }
            if (sceneCam != null)
            {
                s_LeftCam.cullingMask = sceneCam.cullingMask;
                s_LeftCam.clearFlags = sceneCam.clearFlags;
                s_LeftCam.backgroundColor = sceneCam.backgroundColor;
                s_RightCam.cullingMask = sceneCam.cullingMask;
                s_RightCam.clearFlags = sceneCam.clearFlags;
                s_RightCam.backgroundColor = sceneCam.backgroundColor;
            }
        }

        private static void DestroyRenderRig()
        {
            if (s_LeftCam != null) UnityEngine.Object.DestroyImmediate(s_LeftCam.gameObject);
            if (s_RightCam != null) UnityEngine.Object.DestroyImmediate(s_RightCam.gameObject);
            if (s_RigRoot != null) UnityEngine.Object.DestroyImmediate(s_RigRoot);
            s_LeftCam = null;
            s_RightCam = null;
            s_RigRoot = null;

            if (s_LeftRT != null) { s_LeftRT.Release(); UnityEngine.Object.DestroyImmediate(s_LeftRT); }
            if (s_RightRT != null) { s_RightRT.Release(); UnityEngine.Object.DestroyImmediate(s_RightRT); }
            s_LeftRT = null;
            s_RightRT = null;
        }

        private static void RenderEye(Camera cam, RenderTexture rt, float[] viewMat, float[] projMat)
        {
            Matrix4x4 view = ColumnMajorToMatrix4x4(viewMat);
            Matrix4x4 proj = ColumnMajorToMatrix4x4(projMat);

            // Flip projection Y: Metal RenderTextures are Y-inverted and Unity
            // doesn't auto-correct when projectionMatrix is set manually.
            // This also flips det(P), making det(V*P) > 0 — matching Unity's
            // winding convention, so no GL.invertCulling needed.
            proj.m10 = -proj.m10;
            proj.m11 = -proj.m11;
            proj.m12 = -proj.m12;
            proj.m13 = -proj.m13;

            // Reset matrices first so Unity recalculates internal lighting state
            // from the Transform (shadows, light culling, shader built-ins).
            cam.ResetWorldToCameraMatrix();
            cam.ResetProjectionMatrix();

            // Set the camera transform to match the view matrix. Unity's lighting
            // system reads from the Transform, not from worldToCameraMatrix.
            Matrix4x4 viewInv = view.inverse;
            cam.transform.position = new Vector3(viewInv.m03, viewInv.m13, viewInv.m23);
            Vector3 forward = new Vector3(-viewInv.m02, -viewInv.m12, -viewInv.m22);
            Vector3 up = new Vector3(viewInv.m01, viewInv.m11, viewInv.m21);
            cam.transform.rotation = Quaternion.LookRotation(forward, up);

            // Override with exact Kooima matrices for rendering
            cam.worldToCameraMatrix = view;
            cam.projectionMatrix = proj;
            cam.targetTexture = rt;
            cam.Render();
        }
        private static int s_RenderLogOnce;

        private static Matrix4x4 ColumnMajorToMatrix4x4(float[] m)
        {
            var mat = new Matrix4x4();
            mat.m00 = m[0];  mat.m10 = m[1];  mat.m20 = m[2];  mat.m30 = m[3];
            mat.m01 = m[4];  mat.m11 = m[5];  mat.m21 = m[6];  mat.m31 = m[7];
            mat.m02 = m[8];  mat.m12 = m[9];  mat.m22 = m[10]; mat.m32 = m[11];
            mat.m03 = m[12]; mat.m13 = m[13]; mat.m23 = m[14]; mat.m33 = m[15];
            return mat;
        }

        // ================================================================
        // Rig parameter sync (tunables + display pose from scene component)
        // ================================================================

        private static void PushRigParameters()
        {
            // Find DisplayXRDisplay or DisplayXRCamera in the scene
            var displayRig = UnityEngine.Object.FindFirstObjectByType<DisplayXRDisplay>();
            if (displayRig != null)
            {
                float vdh = displayRig.virtualDisplayHeight;
                if (vdh <= 0f && DisplayInfo.isValid)
                    vdh = DisplayInfo.displayHeightMeters;

                Camera rigCam = displayRig.GetComponent<Camera>();
                s_NearZ = rigCam != null ? rigCam.nearClipPlane : 0.3f;
                s_FarZ = rigCam != null ? rigCam.farClipPlane : 1000f;

                if (s_FrameCount % 300 == 0)
                    Debug.Log($"[DisplayXR-SA] Rig: ipd={displayRig.ipdFactor:F2} parallax={displayRig.parallaxFactor:F2} " +
                        $"persp={displayRig.perspectiveFactor:F2} vdh={vdh:F4} (raw={displayRig.virtualDisplayHeight:F4}) " +
                        $"near={s_NearZ:F3} far={s_FarZ:F1} pos={displayRig.transform.position}");

                DisplayXRNative.displayxr_standalone_set_tunables(
                    displayRig.ipdFactor,
                    displayRig.parallaxFactor,
                    displayRig.perspectiveFactor,
                    vdh,
                    0f, // invConvergenceDistance (display-centric = 0)
                    0f, // fovOverride (display-centric = 0)
                    s_NearZ, s_FarZ,
                    0); // cameraCentric = false

                Transform t = displayRig.transform;
                DisplayXRNative.displayxr_standalone_set_display_pose(
                    t.position.x, t.position.y, t.position.z,
                    t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                    t.lossyScale.x, t.lossyScale.y, t.lossyScale.z,
                    1);
                return;
            }

            var cameraRig = UnityEngine.Object.FindFirstObjectByType<DisplayXRCamera>();
            if (cameraRig != null)
            {
                Camera rigCam = cameraRig.GetComponent<Camera>();
                s_NearZ = rigCam != null ? rigCam.nearClipPlane : 0.3f;
                s_FarZ = rigCam != null ? rigCam.farClipPlane : 1000f;
                float halfTanVfov = rigCam != null
                    ? Mathf.Tan(rigCam.fieldOfView * 0.5f * Mathf.Deg2Rad)
                    : 0f;

                DisplayXRNative.displayxr_standalone_set_tunables(
                    cameraRig.ipdFactor,
                    cameraRig.parallaxFactor,
                    1.0f, // perspectiveFactor (camera-centric = 1)
                    0f,   // virtualDisplayHeight (camera-centric = 0)
                    cameraRig.invConvergenceDistance,
                    halfTanVfov,
                    s_NearZ, s_FarZ,
                    1); // cameraCentric = true

                Transform t = cameraRig.transform;
                DisplayXRNative.displayxr_standalone_set_display_pose(
                    t.position.x, t.position.y, t.position.z,
                    t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                    1f, 1f, 1f, // camera-centric: no scale
                    1);
                return;
            }

            // No rig component found — use defaults (identity pose, default tunables)
        }

        // ================================================================
        // Display info / eye tracking / shared texture refresh
        // ================================================================

        private static void RefreshDisplayInfo()
        {
            DisplayXRNative.displayxr_standalone_get_display_info(
                out float wm, out float hm,
                out uint pw, out uint ph,
                out float nx, out float ny, out float nz,
                out float sx, out float sy,
                out int modeSwitch, out int valid);

            DisplayInfo = new DisplayXRDisplayInfo
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
        }

        private static void RefreshEyePositions()
        {
            DisplayXRNative.displayxr_standalone_get_eye_positions(
                out float lx, out float ly, out float lz,
                out float rx, out float ry, out float rz,
                out int tracked);

            LeftEyePosition = new Vector3(lx, ly, lz);
            RightEyePosition = new Vector3(rx, ry, rz);
            IsEyeTracked = tracked != 0;
        }

        private static void RefreshSharedTexture()
        {
            DisplayXRNative.displayxr_standalone_get_shared_texture(
                out IntPtr ptr, out uint w, out uint h, out int ready);
            SharedTextureAvailable = (ready != 0 && ptr != IntPtr.Zero);
        }

        private static string FindRuntimeJson()
        {
            // 1. XR_RUNTIME_JSON environment variable
            string envPath = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            if (!string.IsNullOrEmpty(envPath) && File.Exists(envPath))
                return envPath;

            // 2. Well-known macOS paths
#if UNITY_EDITOR_OSX
            string[] searchPaths = new[]
            {
                // Local development build
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    "Documents/GitHub/openxr-3d-display/_package/DisplayXR-macOS/openxr_displayxr.json"),
                // System-wide active runtime
                "/etc/xdg/openxr/1/active_runtime.json",
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    ".config/openxr/1/active_runtime.json"),
            };

            foreach (string path in searchPaths)
            {
                if (File.Exists(path))
                    return path;
            }
#endif
            return null;
        }

        private static void OnBeforeAssemblyReload()
        {
            if (IsRunning)
            {
                Debug.Log("[DisplayXR-SA] Domain reload: stopping standalone session");
                Stop();
            }
        }

        private static void OnEditorQuitting()
        {
            if (IsRunning)
                Stop();
        }
    }
}
