// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using UnityEngine;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// During Play Mode, opens a fullscreen window on the 3D monitor and blits the
    /// standalone session's weaved output to it each frame for correct lenticular alignment.
    /// Suppresses scene camera rendering in the Game View while the window is active.
    /// Escape key or stopping/pausing Play closes the window.
    /// Camera management (ActiveCamera, cycling) lives in DisplayXRRigManager.
    /// </summary>
    [AddComponentMenu("DisplayXR/Game View Overlay")]
    public class DisplayXRGameViewOverlay : MonoBehaviour
    {
        private bool m_FullscreenShown;

        // Set each frame by DisplayXRPreviewSession (Editor) to the atlas RenderTexture.
        // Contains both eye views side-by-side at the current eye-tracked positions.
        // Using Texture (not Texture2D) so a RenderTexture can be assigned directly —
        // Unity's GUI handles D3D12 orientation automatically for RenderTextures.
        internal static Texture AtlasPreviewTexture;

        // Shared texture handle (kept for cleanup only — not drawn in Game View)
        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;

        // Rendering mode state
        private string[] m_RenderingModeNames;
        private int m_CurrentRenderingMode = 1;

        // Game View display mode: 0=L+R, 1=L only, 2=R only
        private int m_GameViewMode = 0;

        void OnDisable()
        {
            if (m_FullscreenShown)
            {
                DisplayXRNative.displayxr_standalone_fullscreen_window_hide();
                m_FullscreenShown = false;
            }
            AtlasPreviewTexture = null;
            CleanupSharedTexture();
            m_RenderingModeNames = null;
        }

        void Update()
        {
            bool running = DisplayXRNative.displayxr_standalone_is_running() != 0;

            // Fullscreen window lifecycle
            if (running && !m_FullscreenShown)
            {
                if (DisplayXRNative.displayxr_standalone_fullscreen_window_show() != 0)
                    m_FullscreenShown = true;
            }
            else if (!running && m_FullscreenShown)
            {
                DisplayXRNative.displayxr_standalone_fullscreen_window_hide();
                m_FullscreenShown = false;
            }

            // Per-frame: blit shared texture to fullscreen window
            if (m_FullscreenShown)
            {
                DisplayXRNative.displayxr_standalone_fullscreen_window_present();

                if (DisplayXRNative.displayxr_standalone_fullscreen_window_escape_pressed() != 0)
                {
#if UNITY_EDITOR
                    UnityEditor.EditorApplication.ExitPlaymode();
#else
                    Application.Quit();
#endif
                }
            }

            if (running)
                HandleModeKeys();

            HandleFullscreen();
        }

        void OnGUI()
        {
            if (DisplayXRNative.displayxr_standalone_is_running() == 0)
                return;

            // Black background — covers any scene camera output
            GUI.color = Color.black;
            GUI.DrawTexture(new Rect(0, 0, Screen.width, Screen.height), Texture2D.whiteTexture);
            GUI.color = Color.white;

            // AtlasPreviewTexture is the atlas RenderTexture rendered by Unity cameras.
            // Unity's GUI system handles D3D12/Metal orientation automatically for
            // RenderTextures, so no manual Y-flip is needed here.
            Texture atlas = AtlasPreviewTexture;
            if (atlas != null)
            {
                float sw = Screen.width, sh = Screen.height;

                if (m_GameViewMode == 0)
                {
                    // L+R: ScaleToFit letterboxes and centers using the atlas aspect ratio.
                    GUI.DrawTexture(new Rect(0, 0, sw, sh), atlas, ScaleMode.ScaleToFit);
                }
                else
                {
                    // Single eye: half the atlas width gives the single-eye aspect ratio.
                    float aspect = (atlas.width * 0.5f) / atlas.height;
                    float rw, rh, rx, ry;
                    if (sw / sh > aspect) { rh = sh; rw = rh * aspect; rx = (sw - rw) * 0.5f; ry = 0; }
                    else                  { rw = sw; rh = rw / aspect; rx = 0;                ry = (sh - rh) * 0.5f; }

                    // UV: left half (L) or right half (R) of atlas.
                    Rect uv = m_GameViewMode == 1 ? new Rect(0, 0, 0.5f, 1)
                                                  : new Rect(0.5f, 0, 0.5f, 1);
                    GUI.DrawTextureWithTexCoords(new Rect(rx, ry, rw, rh), atlas, uv);
                }
            }

            // HUD: colored mode buttons + render mode + camera name
            string camName  = DisplayXRRigManager.ActiveCameraName ?? "—";
            string modeName = GetCurrentModeName();
            float lx = 8f, ly = 8f;
            string[] viewLabels = { "1:L+R", "2:L", "3:R" };
            for (int i = 0; i < viewLabels.Length; i++)
            {
                GUI.color = (i == m_GameViewMode) ? Color.yellow : new Color(1, 1, 1, 0.6f);
                GUI.Label(new Rect(lx, ly, 52, 20), viewLabels[i], GUI.skin.label);
                lx += 54;
            }
            GUI.color = Color.white;
            GUI.Label(new Rect(lx + 4, ly, Screen.width - lx - 12, 20),
                $"•  Mode: {modeName}  •  Cam: {camName}  •  Esc to stop",
                GUI.skin.label);
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
                m_SharedTexture.filterMode = FilterMode.Point; // No interpolation — preserves interlacing
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
            // F11 is handled by the native window when it is active.
            // Only fall through to Unity's Game View fullscreen toggle when
            // the native window is not showing.
            if (!m_FullscreenShown && GetKeyDown(KeyCode.F11))
                Screen.fullScreen = !Screen.fullScreen;
        }

        // ================================================================
        // Hotkeys: 1/2/3 = Game View mode; V/0/4-8 = rendering mode
        // ================================================================

        private void HandleModeKeys()
        {
            // Game View eye selection (no rendering mode change)
            if (GetKeyDown(KeyCode.Alpha1)) { m_GameViewMode = 0; return; }
            if (GetKeyDown(KeyCode.Alpha2)) { m_GameViewMode = 1; return; }
            if (GetKeyDown(KeyCode.Alpha3)) { m_GameViewMode = 2; return; }

            // Rendering mode switching (V cycles, 0 and 4-8 direct)
            int modeIdx = -1;
            if (GetKeyDown(KeyCode.V))
            {
                EnumerateRenderingModesIfNeeded();
                int count = (m_RenderingModeNames != null && m_RenderingModeNames.Length > 0)
                    ? m_RenderingModeNames.Length : 2;
                modeIdx = (m_CurrentRenderingMode + 1) % count;
            }
            else if (GetKeyDown(KeyCode.Alpha0)) modeIdx = 0;
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
