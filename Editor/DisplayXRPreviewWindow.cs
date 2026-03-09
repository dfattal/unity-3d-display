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
            SharedTexture,
            SideBySide,
            RuntimeReadback,
        }

        [SerializeField] private PreviewSource m_Source = PreviewSource.SharedTexture;
        [SerializeField] private bool m_AutoRefresh = true;

        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;
        private DisplayXRPreview m_CachedPreview;

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
        }

        void OnDisable()
        {
            EditorApplication.update -= OnEditorUpdate;
            CleanupSharedTexture();
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

                // IOSurface/Metal textures are Y-flipped relative to Unity's UV convention
                if (m_Source == PreviewSource.SharedTexture)
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
                EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
                GUILayout.Label($"Display: {info.displayPixelWidth}x{info.displayPixelHeight}  " +
                    $"{info.displayWidthMeters * 100:F1}x{info.displayHeightMeters * 100:F1}cm  " +
                    $"Tracked: {(tracked ? "Yes" : "No")}",
                    EditorStyles.miniLabel);
                EditorGUILayout.EndHorizontal();
            }
        }

        private Texture GetPreviewTexture()
        {
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
