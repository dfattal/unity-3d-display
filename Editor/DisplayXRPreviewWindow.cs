// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Editor window for stereo preview.
    /// Uses the standalone OpenXR session (no play mode required).
    /// Falls back to play-mode preview sources when available.
    /// </summary>
    public class DisplayXRPreviewWindow : EditorWindow
    {
        private enum PreviewSource
        {
            DirectRT,
            SharedTexture,
            SideBySide,
            RuntimeReadback,
        }

        [SerializeField] private PreviewSource m_Source = PreviewSource.DirectRT;
        [SerializeField] private bool m_AutoRefresh = true;

        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;
        private DisplayXRPreview m_CachedPreview;

        // Rendering mode state (mode 0=2D, 1=3D default, 2+=display-specific)
        private string[] m_RenderingModeNames;
        private int m_CurrentRenderingMode = 1; // default to 3D (mode 1)

        [MenuItem("Window/DisplayXR/Preview Window")]
        public static void ShowWindow()
        {
            var gameViewType = typeof(UnityEditor.Editor).Assembly.GetType("UnityEditor.GameView");
            var window = GetWindow<DisplayXRPreviewWindow>("DisplayXR Preview", gameViewType);
            window.minSize = new Vector2(640, 400);
        }

        void OnEnable()
        {
            EditorApplication.update += OnEditorUpdate;
            wantsMouseMove = true; // Ensure the window can receive focus
        }

        void OnDisable()
        {
            EditorApplication.update -= OnEditorUpdate;
            CleanupSharedTexture();
            m_RenderingModeNames = null;
            m_CurrentRenderingMode = 1;
        }

        private void OnEditorUpdate()
        {
            if (m_AutoRefresh && DisplayXRPreviewSession.IsRunning)
                Repaint();
            else if (m_AutoRefresh && Application.isPlaying)
                Repaint();
        }

        void OnGUI()
        {
            // Handle keyboard input only when this window is focused
            HandleKeyInput();

            // Toolbar
            EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);

            // Start/Stop button
            if (DisplayXRPreviewSession.IsRunning)
            {
                if (GUILayout.Button("Stop Preview", EditorStyles.toolbarButton, GUILayout.Width(90)))
                    DisplayXRPreviewSession.Stop();
            }
            else
            {
                if (GUILayout.Button("Start Preview", EditorStyles.toolbarButton, GUILayout.Width(90)))
                    DisplayXRPreviewSession.Start();
            }

            m_Source = (PreviewSource)EditorGUILayout.EnumPopup(m_Source, EditorStyles.toolbarPopup,
                GUILayout.Width(120));
            m_AutoRefresh = GUILayout.Toggle(m_AutoRefresh, "Auto Refresh",
                EditorStyles.toolbarButton, GUILayout.Width(100));
            GUILayout.FlexibleSpace();

            // Status indicator
            if (DisplayXRPreviewSession.IsRunning)
            {
                GUILayout.Label("Runtime: Connected", EditorStyles.toolbarButton);
            }
            else
            {
                // Fall back to checking play-mode feature
                DisplayXRFeature feature = null;
                try { feature = Application.isPlaying ? DisplayXRFeature.Instance : null; }
                catch (Exception) { }

                if (feature != null && feature.DisplayInfo.isValid)
                    GUILayout.Label("Runtime: Connected (Play)", EditorStyles.toolbarButton);
                else
                    GUILayout.Label("Runtime: Not Connected", EditorStyles.toolbarButton);
            }
            EditorGUILayout.EndHorizontal();

            // Preview area
            Rect previewRect = GUILayoutUtility.GetRect(
                GUIContent.none, GUIStyle.none,
                GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));

            Texture tex = GetPreviewTexture();
            if (tex != null)
            {
                float texAspect = (float)tex.width / tex.height;
                float rectAspect = previewRect.width / previewRect.height;

                Rect drawRect;
                if (rectAspect > texAspect)
                {
                    float w = previewRect.height * texAspect;
                    drawRect = new Rect(
                        previewRect.x + (previewRect.width - w) * 0.5f,
                        previewRect.y, w, previewRect.height);
                }
                else
                {
                    float h = previewRect.width / texAspect;
                    drawRect = new Rect(
                        previewRect.x,
                        previewRect.y + (previewRect.height - h) * 0.5f,
                        previewRect.width, h);
                }

                // Metal textures (IOSurface and RenderTexture) are Y-flipped
                if (m_Source == PreviewSource.SharedTexture || m_Source == PreviewSource.DirectRT)
                    GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, 1, 1, -1));
                else
                    GUI.DrawTexture(drawRect, tex, ScaleMode.ScaleToFit);

                var labelRect = new Rect(drawRect.x + 4, drawRect.y + 4, 200, 20);
                GUI.Label(labelRect, $"{tex.width}x{tex.height}", EditorStyles.miniLabel);
            }
            else
            {
                string hint;
                switch (m_Source)
                {
                    case PreviewSource.SharedTexture:
                        hint = DisplayXRPreviewSession.IsRunning
                            ? "Waiting for shared texture..."
                            : "Click 'Start Preview' to begin.";
                        break;
                    case PreviewSource.SideBySide:
                        hint = "Add a DisplayXRPreview component to a camera to see SBS preview.";
                        break;
                    default:
                        hint = "Runtime readback not available.";
                        break;
                }
                EditorGUI.LabelField(previewRect, hint, EditorStyles.centeredGreyMiniLabel);
            }

            // Display info footer
            DisplayXRDisplayInfo info = default;
            bool hasInfo = false;
            bool tracked = false;

            if (DisplayXRPreviewSession.IsRunning && DisplayXRPreviewSession.DisplayInfo.isValid)
            {
                info = DisplayXRPreviewSession.DisplayInfo;
                hasInfo = true;
                tracked = DisplayXRPreviewSession.IsEyeTracked;
            }
            else
            {
                try
                {
                    var feature = Application.isPlaying ? DisplayXRFeature.Instance : null;
                    if (feature != null && feature.DisplayInfo.isValid)
                    {
                        info = feature.DisplayInfo;
                        hasInfo = true;
                        tracked = feature.IsEyeTracked;
                    }
                }
                catch (Exception) { }
            }

            if (hasInfo)
            {
                // Enumerate modes once session is running and display info is available
                if (DisplayXRPreviewSession.IsRunning)
                    EnumerateRenderingModesIfNeeded();

                EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
                string modeName = (m_RenderingModeNames != null &&
                    m_CurrentRenderingMode >= 0 && m_CurrentRenderingMode < m_RenderingModeNames.Length)
                    ? m_RenderingModeNames[m_CurrentRenderingMode]
                    : (m_CurrentRenderingMode <= 0 ? "2D" : "3D");
                int modeCount = m_RenderingModeNames != null ? m_RenderingModeNames.Length : 0;
                GUILayout.Label($"Display: {info.displayPixelWidth}x{info.displayPixelHeight}  " +
                    $"{info.displayWidthMeters * 100:F1}x{info.displayHeightMeters * 100:F1}cm  " +
                    $"Tracked: {(tracked ? "Yes" : "No")}  " +
                    $"Mode: {modeName}",
                    EditorStyles.miniLabel);
                string hint = modeCount > 1
                    ? $"Display modes [0-{modeCount - 1}] | V to Cycle"
                    : "V to Cycle";
                GUILayout.Label(hint, EditorStyles.miniLabel);
                EditorGUILayout.EndHorizontal();
            }
        }

        private void HandleKeyInput()
        {
            if (!DisplayXRPreviewSession.IsRunning) return;

            Event e = Event.current;
            if (e.type != EventType.KeyDown || e.isKey == false) return;

            int modeIdx = -1;

            switch (e.keyCode)
            {
                case KeyCode.V:
                    // Cycle through all rendering modes
                    EnumerateRenderingModesIfNeeded();
                    int count = (m_RenderingModeNames != null && m_RenderingModeNames.Length > 0)
                        ? m_RenderingModeNames.Length : 2;
                    modeIdx = (m_CurrentRenderingMode + 1) % count;
                    break;
                case KeyCode.Alpha0: modeIdx = 0; break;
                case KeyCode.Alpha1: modeIdx = 1; break;
                case KeyCode.Alpha2: modeIdx = 2; break;
                case KeyCode.Alpha3: modeIdx = 3; break;
                case KeyCode.Alpha4: modeIdx = 4; break;
                case KeyCode.Alpha5: modeIdx = 5; break;
                case KeyCode.Alpha6: modeIdx = 6; break;
                case KeyCode.Alpha7: modeIdx = 7; break;
                case KeyCode.Alpha8: modeIdx = 8; break;
            }

            if (modeIdx >= 0)
            {
                EnumerateRenderingModesIfNeeded();
                int maxMode = m_RenderingModeNames != null ? m_RenderingModeNames.Length : 0;
                if (modeIdx < maxMode)
                {
                    DisplayXRNative.displayxr_standalone_request_rendering_mode((uint)modeIdx);
                    m_CurrentRenderingMode = modeIdx;
                    Debug.Log($"[DisplayXR] Rendering mode → {m_RenderingModeNames[modeIdx]}");
                }
                else
                {
                    DisplayXRNative.displayxr_standalone_request_rendering_mode((uint)modeIdx);
                    m_CurrentRenderingMode = modeIdx;
                    Debug.Log($"[DisplayXR] Rendering mode → index {modeIdx}");
                }
                e.Use();
            }
        }

        private void EnumerateRenderingModesIfNeeded()
        {
            if (m_RenderingModeNames != null) return;

            DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                0, out uint count, null, IntPtr.Zero);
            if (count == 0) return;

            uint[] indices = new uint[count];
            m_RenderingModeNames = new string[count];

            // Allocate unmanaged buffer for mode names (256 bytes each)
            IntPtr namesPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)(count * 256));
            try
            {
                DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                    count, out uint fetched, indices, namesPtr);
                for (int i = 0; i < (int)fetched; i++)
                {
                    IntPtr namePtr = new IntPtr(namesPtr.ToInt64() + i * 256);
                    m_RenderingModeNames[i] = System.Runtime.InteropServices.Marshal.PtrToStringAnsi(namePtr) ?? $"mode {i}";
                }
                Debug.Log($"[DisplayXR] Enumerated {fetched} rendering modes: {string.Join(", ", m_RenderingModeNames)}");
            }
            finally
            {
                System.Runtime.InteropServices.Marshal.FreeHGlobal(namesPtr);
            }
        }

        private Texture GetPreviewTexture()
        {
            // Priority 0: Direct RenderTexture (bypass runtime compositor — debug)
            if (m_Source == PreviewSource.DirectRT && DisplayXRPreviewSession.IsRunning
                && DisplayXRPreviewSession.LeftEyeRT != null)
            {
                return DisplayXRPreviewSession.LeftEyeRT;
            }

            // Priority 1: Standalone session shared texture
            if (m_Source == PreviewSource.SharedTexture && DisplayXRPreviewSession.IsRunning
                && DisplayXRPreviewSession.SharedTextureAvailable)
            {
                return UpdateStandaloneSharedTexture();
            }

            // Priority 2: Play-mode preview component (SBS, readback, shared texture via feature)
            if (Application.isPlaying)
            {
                if (m_CachedPreview == null)
                    m_CachedPreview = FindFirstObjectByType<DisplayXRPreview>();
                if (m_CachedPreview == null)
                    return null;

                switch (m_Source)
                {
                    case PreviewSource.SharedTexture:
                        if (m_CachedPreview.mode == DisplayXRPreview.PreviewMode.SharedTexture &&
                            m_CachedPreview.PreviewTexture != null && m_CachedPreview.SharedTextureAvailable)
                            return m_CachedPreview.PreviewTexture;
                        return null;
                    case PreviewSource.RuntimeReadback:
                        if (m_CachedPreview.mode == DisplayXRPreview.PreviewMode.Readback &&
                            m_CachedPreview.PreviewTexture != null && m_CachedPreview.ReadbackAvailable)
                            return m_CachedPreview.PreviewTexture;
                        return null;
                    default:
                        if (m_CachedPreview.mode == DisplayXRPreview.PreviewMode.SideBySide &&
                            m_CachedPreview.PreviewTexture != null)
                            return m_CachedPreview.PreviewTexture;
                        return null;
                }
            }

            m_CachedPreview = null;
            return null;
        }

        private Texture UpdateStandaloneSharedTexture()
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
                m_SharedTexture.name = "DisplayXR_SA_Preview";
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
                DestroyImmediate(m_SharedTexture);
                m_SharedTexture = null;
            }
            m_SharedNativePtr = IntPtr.Zero;
        }
    }
}
