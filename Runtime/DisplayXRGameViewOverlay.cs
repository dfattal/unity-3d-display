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
    /// Suppresses scene camera rendering while the standalone preview is running and draws
    /// the shared texture (IOSurface) via OnGUI. Only one instance needed in the scene.
    /// Camera management (ActiveCamera, cycling) lives in DisplayXRRigManager.
    /// </summary>
    [AddComponentMenu("DisplayXR/Game View Overlay")]
    public class DisplayXRGameViewOverlay : MonoBehaviour
    {
        private struct SuppressedCamera
        {
            public Camera camera;
            public int originalCullingMask;
            public CameraClearFlags originalClearFlags;
            public Color originalBackgroundColor;
        }

        private List<SuppressedCamera> m_SuppressedCameras = new List<SuppressedCamera>();
        private bool m_Suppressing;

        // Shared texture for OnGUI drawing
        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;

        // Rendering mode state
        private string[] m_RenderingModeNames;
        private int m_CurrentRenderingMode = 1;

        void OnDisable()
        {
            RestoreAllCameras();
            CleanupSharedTexture();
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
            }

            if (running)
                HandleModeKeys();

            HandleFullscreen();
        }

        void OnGUI()
        {
            if (DisplayXRNative.displayxr_standalone_is_running() == 0)
                return;

            Texture tex = UpdateSharedTexture();
            if (tex == null) return;

            // Canvas = Game View size in physical pixels
            float backingScale = DisplayXRNative.displayxr_get_backing_scale_factor();
            uint canvasW = (uint)(Screen.width * backingScale);
            uint canvasH = (uint)(Screen.height * backingScale);

            // Tell the runtime the canvas rect in physical pixels
            DisplayXRNative.displayxr_standalone_set_canvas_rect(0, 0, canvasW, canvasH);

            uint surfW = (uint)tex.width;
            uint surfH = (uint)tex.height;

            // UV crop: canvas portion of shared texture
            float uMax = (canvasW > 0 && surfW > 0) ? (float)canvasW / surfW : 1f;
            float vMax = (canvasH > 0 && surfH > 0) ? (float)canvasH / surfH : 1f;

            // Letterbox by canvas aspect (not IOSurface aspect)
            Rect screenRect = new Rect(0, 0, Screen.width, Screen.height);
            float texAspect = (canvasW > 0 && canvasH > 0)
                ? (float)canvasW / canvasH
                : (float)surfW / surfH;
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

            // Metal textures are Y-flipped; D3D12/D3D11 are not
#if UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX
            GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, vMax, uMax, -vMax));
#else
            GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, 0, uMax, vMax));
#endif

            // Status label
            string modeName = GetCurrentModeName();
            string camName = DisplayXRRigManager.ActiveCameraName ?? "—";
            GUI.Label(new Rect(drawRect.x + 4, drawRect.y + 4, 800, 20),
                $"Canvas: {canvasW}x{canvasH}  Surface: {surfW}x{surfH}  Mode: {modeName}  Camera: {camName}",
                GUI.skin.label);
        }

        // ================================================================
        // Camera suppression (prevent scene rendering while preview runs)
        // ================================================================

        private void SuppressSceneRendering()
        {
            m_SuppressedCameras.Clear();
            var registered = DisplayXRRigManager.RegisteredCameras;
            for (int i = 0; i < registered.Count; i++)
            {
                var cam = registered[i];
                if (cam == null) continue;
                m_SuppressedCameras.Add(new SuppressedCamera
                {
                    camera = cam,
                    originalCullingMask = cam.cullingMask,
                    originalClearFlags = cam.clearFlags,
                    originalBackgroundColor = cam.backgroundColor,
                });
                cam.cullingMask = 0;
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.black;
            }
        }

        private void RestoreAllCameras()
        {
            for (int i = 0; i < m_SuppressedCameras.Count; i++)
            {
                var mc = m_SuppressedCameras[i];
                if (mc.camera == null) continue;
                mc.camera.cullingMask = mc.originalCullingMask;
                mc.camera.clearFlags = mc.originalClearFlags;
                mc.camera.backgroundColor = mc.originalBackgroundColor;
            }
            m_SuppressedCameras.Clear();
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
                    (int)w, (int)h, TextureFormat.RGBA32, false, true, nativePtr);
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

        private void HandleFullscreen()
        {
            if (GetKeyDown(KeyCode.F11))
                Screen.fullScreen = !Screen.fullScreen;
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
                case KeyCode.F11: return Key.F11;
                default: return Key.None;
            }
        }
#else
        private static bool GetKeyDown(KeyCode k) => Input.GetKeyDown(k);
#endif
    }
}
