// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using UnityEngine;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// Renders the DisplayXR composited preview texture in the Game View during Play Mode.
    /// Auto-assigns targetDisplay to each discovered DisplayXR camera so Unity's Display
    /// dropdown acts as a camera selector. Only one instance needed in the scene.
    /// </summary>
    [AddComponentMenu("DisplayXR/Game View Overlay")]
    public class DisplayXRGameViewOverlay : MonoBehaviour
    {
        /// <summary>
        /// The currently active camera as determined by Display dropdown selection.
        /// Read by DisplayXRPreviewSession.PushRigParameters() to override the source camera.
        /// </summary>
        public static Camera ActiveCamera { get; private set; }

        private struct ManagedCamera
        {
            public Camera camera;
            public int originalCullingMask;
            public CameraClearFlags originalClearFlags;
            public Color originalBackgroundColor;
            public int assignedDisplay;
            public bool hasDisplayRig;
            public bool hasCameraRig;
        }

        private List<ManagedCamera> m_ManagedCameras = new List<ManagedCamera>();
        private bool m_Suppressing;

        // Shared texture for OnGUI drawing
        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;

        // Rendering mode state
        private string[] m_RenderingModeNames;
        private int m_CurrentRenderingMode = 1;

        // Track active camera via onPreRender
        private static Camera s_LastPreRenderCamera;

        void Start()
        {
            DiscoverAndAssignCameras();
            Camera.onPreRender += OnCameraPreRender;
        }

        void OnDisable()
        {
            Camera.onPreRender -= OnCameraPreRender;
            RestoreAllCameras();
            CleanupSharedTexture();
            ActiveCamera = null;
            s_LastPreRenderCamera = null;
            m_RenderingModeNames = null;
        }

        void Update()
        {
            bool running = DisplayXRNative.displayxr_standalone_is_running() != 0;

            if (running && !m_Suppressing)
            {
                SuppressSceneRendering();
                m_Suppressing = true;
            }
            else if (!running && m_Suppressing)
            {
                RestoreAllCameras();
                m_Suppressing = false;
                ActiveCamera = null;
            }

            if (running)
            {
                UpdateActiveCamera();
                HandleModeKeys();
            }
        }

        void OnGUI()
        {
            if (DisplayXRNative.displayxr_standalone_is_running() == 0)
                return;

            Texture tex = UpdateSharedTexture();
            if (tex == null) return;

            // Fullscreen with aspect-ratio letterboxing
            Rect screenRect = new Rect(0, 0, Screen.width, Screen.height);
            float texAspect = (float)tex.width / tex.height;
            float screenAspect = screenRect.width / screenRect.height;

            Rect drawRect;
            if (screenAspect > texAspect)
            {
                float w = screenRect.height * texAspect;
                drawRect = new Rect((screenRect.width - w) * 0.5f, 0, w, screenRect.height);
            }
            else
            {
                float h = screenRect.width / texAspect;
                drawRect = new Rect(0, (screenRect.height - h) * 0.5f, screenRect.width, h);
            }

            // Y-flip for Metal (IOSurface)
            GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, 1, 1, -1));

            // Status label
            string modeName = GetCurrentModeName();
            string camName = ActiveCamera != null ? ActiveCamera.gameObject.name : "None";
            GUI.Label(new Rect(drawRect.x + 4, drawRect.y + 4, 400, 20),
                $"{tex.width}x{tex.height}  Mode: {modeName}  Camera: {camName}",
                GUI.skin.label);
        }

        // ================================================================
        // Camera discovery and display assignment
        // ================================================================

        private void DiscoverAndAssignCameras()
        {
            m_ManagedCameras.Clear();
            var cameras = FindObjectsByType<Camera>(FindObjectsSortMode.None);
            var entries = new List<ManagedCamera>();

            foreach (var cam in cameras)
            {
                if (cam.gameObject.hideFlags.HasFlag(HideFlags.HideAndDontSave))
                    continue;

                var entry = new ManagedCamera
                {
                    camera = cam,
                    originalCullingMask = cam.cullingMask,
                    originalClearFlags = cam.clearFlags,
                    originalBackgroundColor = cam.backgroundColor,
                    hasDisplayRig = cam.GetComponent<DisplayXRDisplay>() != null,
                    hasCameraRig = cam.GetComponent<DisplayXRCamera>() != null,
                };

                // Only manage cameras with DisplayXR components
                if (entry.hasDisplayRig || entry.hasCameraRig)
                    entries.Add(entry);
            }

            // Sort: DisplayRig first, then CameraRig
            entries.Sort((a, b) =>
            {
                int catA = a.hasDisplayRig ? 0 : 1;
                int catB = b.hasDisplayRig ? 0 : 1;
                int catCmp = catA.CompareTo(catB);
                if (catCmp != 0) return catCmp;
                return string.Compare(a.camera.name, b.camera.name, StringComparison.Ordinal);
            });

            // Assign sequential targetDisplay values
            for (int i = 0; i < entries.Count; i++)
            {
                var e = entries[i];
                e.assignedDisplay = i;
                e.camera.targetDisplay = i;
                entries[i] = e;

                string rigType = e.hasDisplayRig ? "Display" : "Camera";
                Debug.Log($"[DisplayXR] Display {i + 1} = {e.camera.name} ({rigType})");
            }

            m_ManagedCameras = entries;

            // Set initial active camera
            if (m_ManagedCameras.Count > 0)
                ActiveCamera = m_ManagedCameras[0].camera;
        }

        // ================================================================
        // Camera suppression (prevent scene rendering, save GPU)
        // ================================================================

        private void SuppressSceneRendering()
        {
            for (int i = 0; i < m_ManagedCameras.Count; i++)
            {
                var mc = m_ManagedCameras[i];
                if (mc.camera == null) continue;
                mc.camera.cullingMask = 0;
                mc.camera.clearFlags = CameraClearFlags.SolidColor;
                mc.camera.backgroundColor = Color.black;
            }
        }

        private void RestoreAllCameras()
        {
            for (int i = 0; i < m_ManagedCameras.Count; i++)
            {
                var mc = m_ManagedCameras[i];
                if (mc.camera == null) continue;
                mc.camera.cullingMask = mc.originalCullingMask;
                mc.camera.clearFlags = mc.originalClearFlags;
                mc.camera.backgroundColor = mc.originalBackgroundColor;
            }
        }

        // ================================================================
        // Active camera tracking via Display dropdown
        // ================================================================

        private static void OnCameraPreRender(Camera cam)
        {
            s_LastPreRenderCamera = cam;
        }

        private void UpdateActiveCamera()
        {
            if (s_LastPreRenderCamera == null) return;

            // Check if the rendering camera is one of ours
            for (int i = 0; i < m_ManagedCameras.Count; i++)
            {
                if (m_ManagedCameras[i].camera == s_LastPreRenderCamera)
                {
                    if (ActiveCamera != s_LastPreRenderCamera)
                    {
                        ActiveCamera = s_LastPreRenderCamera;
                        Debug.Log($"[DisplayXR] Active camera switched to: {ActiveCamera.name} (Display {i + 1})");
                    }
                    break;
                }
            }
        }

        // ================================================================
        // Shared texture (IOSurface) for Game View rendering
        // ================================================================

        private Texture UpdateSharedTexture()
        {
            DisplayXRNative.displayxr_standalone_get_shared_texture(
                out IntPtr nativePtr, out uint w, out uint h, out int ready);

            if (ready == 0 || nativePtr == IntPtr.Zero)
                return null;

            if (m_SharedTexture == null || m_SharedNativePtr != nativePtr ||
                m_SharedTexture.width != (int)w || m_SharedTexture.height != (int)h)
            {
                CleanupSharedTexture();
                m_SharedTexture = Texture2D.CreateExternalTexture(
                    (int)w, (int)h, TextureFormat.BGRA32, false, false, nativePtr);
                m_SharedTexture.name = "DisplayXR_GameView";
                m_SharedTexture.filterMode = FilterMode.Bilinear;
                m_SharedNativePtr = nativePtr;
            }
            else
            {
                m_SharedTexture.UpdateExternalTexture(nativePtr);
            }

            return m_SharedTexture;
        }

        private void CleanupSharedTexture()
        {
            if (m_SharedTexture != null)
            {
                Destroy(m_SharedTexture);
                m_SharedTexture = null;
            }
            m_SharedNativePtr = IntPtr.Zero;
        }

        // ================================================================
        // Rendering mode hotkeys (V / 0-8)
        // ================================================================

        private void HandleModeKeys()
        {
            int modeIdx = -1;

            if (GetKeyDown(KeyCode.V))
            {
                EnumerateRenderingModesIfNeeded();
                int count = (m_RenderingModeNames != null && m_RenderingModeNames.Length > 0)
                    ? m_RenderingModeNames.Length : 2;
                modeIdx = (m_CurrentRenderingMode + 1) % count;
            }
            else if (GetKeyDown(KeyCode.Alpha0)) modeIdx = 0;
            else if (GetKeyDown(KeyCode.Alpha1)) modeIdx = 1;
            else if (GetKeyDown(KeyCode.Alpha2)) modeIdx = 2;
            else if (GetKeyDown(KeyCode.Alpha3)) modeIdx = 3;
            else if (GetKeyDown(KeyCode.Alpha4)) modeIdx = 4;
            else if (GetKeyDown(KeyCode.Alpha5)) modeIdx = 5;
            else if (GetKeyDown(KeyCode.Alpha6)) modeIdx = 6;
            else if (GetKeyDown(KeyCode.Alpha7)) modeIdx = 7;
            else if (GetKeyDown(KeyCode.Alpha8)) modeIdx = 8;

            if (modeIdx >= 0)
            {
                EnumerateRenderingModesIfNeeded();
                DisplayXRNative.displayxr_standalone_request_rendering_mode((uint)modeIdx);
                m_CurrentRenderingMode = modeIdx;
                string name = (m_RenderingModeNames != null && modeIdx < m_RenderingModeNames.Length)
                    ? m_RenderingModeNames[modeIdx] : $"index {modeIdx}";
                Debug.Log($"[DisplayXR] Rendering mode → {name}");
            }
        }

        private void EnumerateRenderingModesIfNeeded()
        {
            if (m_RenderingModeNames != null) return;

            DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                0, out uint count, null, IntPtr.Zero,
                null, null, null, null, null, null, null, null);
            if (count == 0) return;

            uint[] indices = new uint[count];
            m_RenderingModeNames = new string[count];
            uint[] viewCounts = new uint[count];
            uint[] tileCols = new uint[count];
            uint[] tileRows = new uint[count];
            uint[] viewWidths = new uint[count];
            uint[] viewHeights = new uint[count];
            float[] scaleX = new float[count];
            float[] scaleY = new float[count];
            int[] hw3d = new int[count];

            IntPtr namesPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)(count * 256));
            try
            {
                DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                    count, out uint fetched, indices, namesPtr,
                    viewCounts, tileCols, tileRows,
                    viewWidths, viewHeights,
                    scaleX, scaleY, hw3d);
                for (int i = 0; i < (int)fetched; i++)
                {
                    IntPtr namePtr = new IntPtr(namesPtr.ToInt64() + i * 256);
                    m_RenderingModeNames[i] = System.Runtime.InteropServices.Marshal.PtrToStringAnsi(namePtr) ?? $"mode {i}";
                }
            }
            finally
            {
                System.Runtime.InteropServices.Marshal.FreeHGlobal(namesPtr);
            }
        }

        private string GetCurrentModeName()
        {
            EnumerateRenderingModesIfNeeded();
            if (m_RenderingModeNames != null &&
                m_CurrentRenderingMode >= 0 && m_CurrentRenderingMode < m_RenderingModeNames.Length)
                return m_RenderingModeNames[m_CurrentRenderingMode];
            return m_CurrentRenderingMode <= 0 ? "2D" : "3D";
        }

        // ================================================================
        // Input abstraction (matches DisplayXRInputController pattern)
        // ================================================================

#if HAS_INPUT_SYSTEM
        private static bool GetKeyDown(KeyCode k) =>
            Keyboard.current != null && Keyboard.current[ToKey(k)].wasPressedThisFrame;

        private static Key ToKey(KeyCode k)
        {
            switch (k)
            {
                case KeyCode.V: return Key.V;
                case KeyCode.Alpha0: return Key.Digit0;
                case KeyCode.Alpha1: return Key.Digit1;
                case KeyCode.Alpha2: return Key.Digit2;
                case KeyCode.Alpha3: return Key.Digit3;
                case KeyCode.Alpha4: return Key.Digit4;
                case KeyCode.Alpha5: return Key.Digit5;
                case KeyCode.Alpha6: return Key.Digit6;
                case KeyCode.Alpha7: return Key.Digit7;
                case KeyCode.Alpha8: return Key.Digit8;
                default: return Key.None;
            }
        }
#else
        private static bool GetKeyDown(KeyCode k) => Input.GetKeyDown(k);
#endif
    }
}
