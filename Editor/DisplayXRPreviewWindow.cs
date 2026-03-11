// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using DisplayXR;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Editor window for stereo preview (SBS or readback mode).
    /// Docks next to the Game view by default.
    /// </summary>
    public class DisplayXRPreviewWindow : EditorWindow
    {
        private enum PreviewSource
        {
            SideBySide,
            RuntimeReadback,
            SharedTexture,
        }

        [SerializeField] private PreviewSource m_Source = PreviewSource.SideBySide;
        [SerializeField] private bool m_AutoRefresh = true;

        private Texture2D m_PreviewTexture;
        private DisplayXRPreview m_CachedPreview;
        private bool m_ExitingPlayMode;

        [MenuItem("Window/DisplayXR/Preview Window")]
        public static void ShowWindow()
        {
            // Dock next to Game view by default so it stays inside the editor layout
            var gameViewType = typeof(UnityEditor.Editor).Assembly.GetType("UnityEditor.GameView");
            var window = GetWindow<DisplayXRPreviewWindow>("DisplayXR Preview", gameViewType);
            window.minSize = new Vector2(640, 400);
        }

        void OnEnable()
        {
            EditorApplication.playModeStateChanged += OnPlayModeChanged;
            EditorApplication.update += OnEditorUpdate;
        }

        void OnDisable()
        {
            EditorApplication.playModeStateChanged -= OnPlayModeChanged;
            EditorApplication.update -= OnEditorUpdate;
        }

        private void OnPlayModeChanged(PlayModeStateChange state)
        {
            if (state == PlayModeStateChange.ExitingPlayMode)
            {
                m_CachedPreview = null;
                m_ExitingPlayMode = true;

            }
            else if (state == PlayModeStateChange.EnteredEditMode)
            {
                m_ExitingPlayMode = false;
            }
            else if (state == PlayModeStateChange.EnteredPlayMode)
            {
                // Unity auto-focuses the Game tab on play. Switch back to
                // our preview tab so the user sees the composited output.
                EditorApplication.delayCall += () =>
                {
                    if (this != null)
                        Focus();
                };
            }
        }

        private void OnEditorUpdate()
        {
            if (m_AutoRefresh && Application.isPlaying && !m_ExitingPlayMode)
                Repaint();
        }

        void OnGUI()
        {
            // Toolbar
            EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
            m_Source = (PreviewSource)EditorGUILayout.EnumPopup(m_Source, EditorStyles.toolbarPopup,
                GUILayout.Width(150));
            m_AutoRefresh = GUILayout.Toggle(m_AutoRefresh, "Auto Refresh",
                EditorStyles.toolbarButton, GUILayout.Width(100));
            GUILayout.FlexibleSpace();

            // Status indicator — guard against accessing destroyed singletons during teardown
            DisplayXRFeature feature = null;
            try { feature = Application.isPlaying ? DisplayXRFeature.Instance : null; }
            catch (System.Exception) { /* destroyed during teardown */ }

            if (feature != null && feature.DisplayInfo.isValid)
            {
                GUILayout.Label("Runtime: Connected", EditorStyles.toolbarButton);
            }
            else
            {
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
                // Maintain aspect ratio
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

                // IOSurface/Metal textures are Y-flipped relative to Unity's UV convention
                if (m_Source == PreviewSource.SharedTexture)
                    GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, 1, 1, -1));
                else
                    GUI.DrawTexture(drawRect, tex, ScaleMode.ScaleToFit);

                // Label
                var labelRect = new Rect(drawRect.x + 4, drawRect.y + 4, 200, 20);
                GUI.Label(labelRect, $"{tex.width}x{tex.height}", EditorStyles.miniLabel);
            }
            else
            {
                string hint;
                switch (m_Source)
                {
                    case PreviewSource.SideBySide:
                        hint = "Add a DisplayXRPreview component to a camera to see SBS preview.";
                        break;
                    case PreviewSource.SharedTexture:
                        hint = "Shared texture not available. Ensure runtime supports GPU texture sharing (macOS).";
                        break;
                    default:
                        hint = "Runtime readback not available. Ensure DisplayXR is running with offscreen mode.";
                        break;
                }
                EditorGUI.LabelField(previewRect, hint, EditorStyles.centeredGreyMiniLabel);
            }

            // Display info footer
            if (feature != null && feature.DisplayInfo.isValid)
            {
                var info = feature.DisplayInfo;
                EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
                GUILayout.Label($"Display: {info.displayPixelWidth}x{info.displayPixelHeight}  " +
                    $"{info.displayWidthMeters * 100:F1}x{info.displayHeightMeters * 100:F1}cm  " +
                    $"Tracked: {(feature.IsEyeTracked ? "Yes" : "No")}",
                    EditorStyles.miniLabel);
                EditorGUILayout.EndHorizontal();
            }
        }

        private Texture GetPreviewTexture()
        {
            // Don't search for scene objects during teardown — causes recursive GUI
            if (!Application.isPlaying)
            {
                m_CachedPreview = null;
                return null;
            }

            // Cache the preview component to avoid FindFirstObjectByType inside OnGUI
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

                default: // SideBySide
                    if (m_CachedPreview.mode == DisplayXRPreview.PreviewMode.SideBySide &&
                        m_CachedPreview.PreviewTexture != null)
                        return m_CachedPreview.PreviewTexture;
                    return null;
            }
        }
    }
}
