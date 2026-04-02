// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Runtime.InteropServices;
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

        // Rendering mode state
        private string[] m_RenderingModeNames;
        private int m_CurrentRenderingMode = 1;

        // Game View display mode: 0=L+R, 1=L only, 2=R only
        private int m_GameViewMode = 0;

        // Last known-good tile layout (cached so windowed mode can reuse fullscreen values)
        private uint m_CachedTileCols, m_CachedTileRows, m_CachedViewW, m_CachedViewH;

        void OnDisable()
        {
            if (m_FullscreenShown)
            {
                DisplayXRNative.displayxr_standalone_fullscreen_window_hide();
                m_FullscreenShown = false;
            }
            AtlasPreviewTexture = null;
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

            Texture atlas = AtlasPreviewTexture;
            if (atlas != null && atlas.width > 0 && atlas.height > 0)
            {
                float sw = Screen.width, sh = Screen.height;

                DisplayXRNative.displayxr_standalone_get_current_mode_info(
                    out _, out uint tileCols, out uint tileRows,
                    out uint viewW, out uint viewH,
                    out _, out _, out _);

                // Get HWND client size early — needed for windowed tileInfoValid check.
#if UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN
                Vector2Int hwndSize = GetDisplayXRWindowClientSize();
#else
                Vector2Int hwndSize = Vector2Int.zero;
#endif

                // Tile info is valid when:
                //   (a) tileCols*viewW == atlas.width — fullscreen, content fills atlas width
                //   (b) m_FullscreenShown — window is running, so native values are real.
                //       In windowed mode tileCols*viewW < atlas.width (atlas is always full
                //       display size) but the values are still correct for UV cropping.
                //       The 250×250 startup placeholders only appear before the window opens.
                // Using m_FullscreenShown avoids brittle HWND-size matching that breaks during
                // window resize (HWND updates immediately but native lags a frame or two).
                bool tileInfoValid = tileRows > 0 && tileCols > 0 && viewW > 0 && viewH > 0
                    && (tileCols * viewW == (uint)atlas.width || m_FullscreenShown);
                if (tileInfoValid)
                {
                    m_CachedTileCols = tileCols;
                    m_CachedTileRows = tileRows;
                    m_CachedViewW    = viewW;
                    m_CachedViewH    = viewH;
                }
                else if (m_CachedTileCols > 0 && m_CachedTileRows > 0)
                {
                    tileCols = m_CachedTileCols;
                    tileRows = m_CachedTileRows;
                    viewW    = m_CachedViewW;
                    viewH    = m_CachedViewH;
                    tileInfoValid = true;
                }

                // Canvas = the tile region Unity cameras actually rendered into the atlas.
                // Use tile dims (not HWND) so only the real content rows are sampled —
                // the HWND can be taller than one tile row when the compositor uses
                // additional rows for its own purposes.
                float canvasW, canvasH;
                if (tileInfoValid)
                {
                    canvasW = tileCols * viewW;
                    canvasH = tileRows * viewH;
                }
                else if (hwndSize.x > 0 && hwndSize.y > 0)
                {
                    // Tile info unavailable — fall back to HWND as best estimate
                    canvasW = hwndSize.x;
                    canvasH = hwndSize.y;
                }
                else
                {
                    canvasW = atlas.width;
                    canvasH = atlas.height;
                }

                float uWidthFrac = Mathf.Min(canvasW / atlas.width,  1f);
                float vMax       = Mathf.Min(canvasH / atlas.height, 1f);

                // Atlas is Y-flipped (projection flip corrects weaver input on D3D12).
                // Compensate by sampling from vMax downward (negative height = V flip).
                Rect uvRect = m_GameViewMode == 2 ? new Rect(uWidthFrac * 0.5f, vMax, uWidthFrac * 0.5f, -vMax)
                            : m_GameViewMode == 1 ? new Rect(0f,                vMax, uWidthFrac * 0.5f, -vMax)
                            :                       new Rect(0f,                vMax, uWidthFrac,         -vMax);

                float displayW      = m_GameViewMode == 0 ? canvasW : canvasW * 0.5f;
                float contentAspect = displayW / canvasH;

                // object-fit: contain — scale to fit sw×sh, preserve aspect, center.
                float rw, rh;
                if (sw / sh > contentAspect) { rh = sh; rw = rh * contentAspect; }
                else                         { rw = sw; rh = rw / contentAspect; }
                float rx = (sw - rw) * 0.5f;
                float ry = (sh - rh) * 0.5f;

                GUI.DrawTextureWithTexCoords(new Rect(rx, ry, rw, rh), atlas, uvRect);
            }

            // HUD: colored mode buttons + render mode + camera name
            string camName  = DisplayXRRigManager.ActiveCameraName ?? "—";
            string modeName = GetCurrentModeName();
            GUIStyle hudStyle = new GUIStyle(GUI.skin.label) { fontSize = 16 };
            float lx = 8f, ly = 6f;
            string[] viewLabels = { "SBS (1)", "Left (2)", "Right (3)" };
            float[] viewWidths  = { 72f, 68f, 72f };
            for (int i = 0; i < viewLabels.Length; i++)
            {
                GUI.color = (i == m_GameViewMode) ? Color.yellow : new Color(1, 1, 1, 0.6f);
                GUI.Label(new Rect(lx, ly, viewWidths[i], 24), viewLabels[i], hudStyle);
                lx += viewWidths[i] + 4;
            }
            GUI.color = Color.white;
            GUI.Label(new Rect(lx + 4, ly, Screen.width - lx - 12, 24),
                $"•  Mode: {modeName}  •  Cam: {camName}",
                hudStyle);
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

        // ================================================================
        // Windowed HWND size query (Windows only, no native plugin changes)
        // ================================================================

#if UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int Left, Top, Right, Bottom; }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

        [DllImport("user32.dll")]
        private static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

        // Returns the DisplayXR fullscreen window's client area, or (0,0) if not found.
        private static Vector2Int GetDisplayXRWindowClientSize()
        {
            IntPtr hwnd = FindWindowW("DisplayXR_Fullscreen", null);
            if (hwnd == IntPtr.Zero) return Vector2Int.zero;
            if (!GetClientRect(hwnd, out RECT r)) return Vector2Int.zero;
            int w = r.Right  - r.Left;
            int h = r.Bottom - r.Top;
            return w > 0 && h > 0 ? new Vector2Int(w, h) : Vector2Int.zero;
        }
#endif
    }
}
