// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using Monado.Display3D;

namespace Monado.Display3D.Editor
{
    /// <summary>
    /// Editor window for stereo preview (SBS or readback mode).
    /// </summary>
    public class Monado3DPreviewWindow : EditorWindow
    {
        private enum PreviewSource
        {
            SideBySide,
            RuntimeReadback,
        }

        [SerializeField] private PreviewSource m_Source = PreviewSource.SideBySide;
        [SerializeField] private bool m_AutoRefresh = true;

        private Texture2D m_PreviewTexture;

        [MenuItem("Window/Monado3D/Preview Window")]
        public static void ShowWindow()
        {
            var window = GetWindow<Monado3DPreviewWindow>("Monado3D Preview");
            window.minSize = new Vector2(640, 400);
        }

        void OnEnable()
        {
            EditorApplication.update += OnEditorUpdate;
        }

        void OnDisable()
        {
            EditorApplication.update -= OnEditorUpdate;
        }

        private void OnEditorUpdate()
        {
            if (m_AutoRefresh && Application.isPlaying)
            {
                Repaint();
            }
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

            // Status indicator
            var feature = Monado3DFeature.Instance;
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

                GUI.DrawTexture(drawRect, tex, ScaleMode.ScaleToFit);

                // Label
                var labelRect = new Rect(drawRect.x + 4, drawRect.y + 4, 200, 20);
                GUI.Label(labelRect, $"{tex.width}x{tex.height}", EditorStyles.miniLabel);
            }
            else
            {
                EditorGUI.LabelField(previewRect,
                    m_Source == PreviewSource.SideBySide
                        ? "Add a Monado3DPreview component to a camera to see SBS preview."
                        : "Runtime readback not available. Ensure Monado is running with offscreen mode.",
                    EditorStyles.centeredGreyMiniLabel);
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
            if (m_Source == PreviewSource.RuntimeReadback)
            {
                // Find Monado3DPreview in readback mode
                var preview = FindFirstObjectByType<Monado3DPreview>();
                if (preview != null && preview.mode == Monado3DPreview.PreviewMode.Readback &&
                    preview.PreviewTexture != null && preview.ReadbackAvailable)
                {
                    return preview.PreviewTexture;
                }
                return null;
            }

            // SBS: find Monado3DPreview in SBS mode
            var sbsPreview = FindFirstObjectByType<Monado3DPreview>();
            if (sbsPreview != null && sbsPreview.mode == Monado3DPreview.PreviewMode.SideBySide &&
                sbsPreview.PreviewTexture != null)
            {
                return sbsPreview.PreviewTexture;
            }

            return null;
        }
    }
}
