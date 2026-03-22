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
    /// Displays the runtime's shared texture (IOSurface) after compositing.
    /// </summary>
    public class DisplayXRPreviewWindow : EditorWindow
    {
        [SerializeField] private bool m_AutoRefresh = true;

        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;

        // Camera selector
        private CameraEntry[] m_CameraList;
        private string[] m_CameraNames;
        private int m_SelectedCameraIndex;
        private double m_LastCameraRefreshTime;

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
            EditorApplication.hierarchyChanged += OnHierarchyChanged;
            wantsMouseMove = true; // Ensure the window can receive focus
            RefreshCameraList();
        }

        void OnDisable()
        {
            EditorApplication.update -= OnEditorUpdate;
            EditorApplication.hierarchyChanged -= OnHierarchyChanged;
            CleanupSharedTexture();
            m_RenderingModeNames = null;
            m_ModeViewCounts = null;
            m_ModeTileColumns = null;
            m_ModeTileRows = null;
            m_ModeViewWidths = null;
            m_ModeViewHeights = null;
            m_CurrentRenderingMode = 1;
        }

        private void OnHierarchyChanged()
        {
            RefreshCameraList();
        }

        private void OnEditorUpdate()
        {
            if (m_AutoRefresh && DisplayXRPreviewSession.IsRunning)
                Repaint();

            // Throttled camera list refresh (1x/second)
            double now = EditorApplication.timeSinceStartup;
            if (now - m_LastCameraRefreshTime > 1.0)
            {
                m_LastCameraRefreshTime = now;
                ValidateCameraSelection();
            }
        }

        private void RefreshCameraList()
        {
            m_CameraList = DisplayXRPreviewSession.DiscoverCameras();
            m_CameraNames = new string[m_CameraList.Length];
            for (int i = 0; i < m_CameraList.Length; i++)
                m_CameraNames[i] = m_CameraList[i].displayName;

            // Sync index to current selection
            m_SelectedCameraIndex = 0;
            Camera sel = DisplayXRPreviewSession.SelectedCamera;
            if (sel != null)
            {
                for (int i = 0; i < m_CameraList.Length; i++)
                {
                    if (m_CameraList[i].camera == sel)
                    {
                        m_SelectedCameraIndex = i;
                        break;
                    }
                }
            }
        }

        private void ValidateCameraSelection()
        {
            Camera sel = DisplayXRPreviewSession.SelectedCamera;
            if (sel == null && DisplayXRPreviewSession.IsRunning)
            {
                // Selected camera was destroyed — fall back
                RefreshCameraList();
                DisplayXRPreviewSession.RestoreSelection();
                RefreshCameraList(); // re-sync index after restore
            }
        }

        void OnGUI()
        {
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
                {
                    RefreshCameraList();
                    DisplayXRPreviewSession.Start();
                    RefreshCameraList(); // re-sync after session starts and restores selection
                }
            }

            // Camera selector dropdown
            if (m_CameraNames != null && m_CameraNames.Length > 0)
            {
                int newIdx = EditorGUILayout.Popup(m_SelectedCameraIndex, m_CameraNames,
                    EditorStyles.toolbarPopup, GUILayout.Width(150));
                if (newIdx != m_SelectedCameraIndex && newIdx >= 0 && newIdx < m_CameraList.Length)
                {
                    m_SelectedCameraIndex = newIdx;
                    var entry = m_CameraList[newIdx];
                    DisplayXRPreviewSession.SelectCamera(entry.camera);
                    // Auto-switch rendering mode: regular camera → 2D, rigs → 3D
                    if (entry.category == CameraCategory.RegularCamera)
                    {
                        if (m_CurrentRenderingMode != 0)
                        {
                            DisplayXRNative.displayxr_standalone_request_rendering_mode(0);
                            m_CurrentRenderingMode = 0;
                        }
                    }
                    else
                    {
                        if (m_CurrentRenderingMode == 0)
                        {
                            DisplayXRNative.displayxr_standalone_request_rendering_mode(1);
                            m_CurrentRenderingMode = 1;
                        }
                    }
                }
            }

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
                GUILayout.Label("Runtime: Not Connected", EditorStyles.toolbarButton);
            }
            EditorGUILayout.EndHorizontal();

            // Preview area
            Rect previewRect = GUILayoutUtility.GetRect(
                GUIContent.none, GUIStyle.none,
                GUILayout.ExpandWidth(true), GUILayout.ExpandHeight(true));

            // Canvas = preview area in backing pixels (Retina-aware)
            float ppp = EditorGUIUtility.pixelsPerPoint;
            uint canvasW = (uint)(previewRect.width * ppp);
            uint canvasH = (uint)(previewRect.height * ppp);

            // Tell the runtime where the canvas is (screen position + size)
            if (DisplayXRPreviewSession.IsRunning)
            {
                Rect winPos = position;
                int screenX = (int)((winPos.x + previewRect.x) * ppp);
                int screenY = (int)((winPos.y + previewRect.y) * ppp);
                DisplayXRNative.displayxr_standalone_set_canvas_rect(
                    screenX, screenY, canvasW, canvasH);
            }

            Texture tex = GetPreviewTexture();
            if (tex != null)
            {
                uint surfW = (uint)tex.width;
                uint surfH = (uint)tex.height;

                // UV crop: canvas portion of IOSurface (compositor writes to top-left)
                float uMax = (canvasW > 0 && surfW > 0) ? (float)canvasW / surfW : 1f;
                float vMax = (canvasH > 0 && surfH > 0) ? (float)canvasH / surfH : 1f;

                // Letterbox by canvas aspect (not IOSurface aspect)
                float texAspect = (canvasW > 0 && canvasH > 0)
                    ? (float)canvasW / canvasH
                    : (float)surfW / surfH;
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

                // Metal textures are Y-flipped; crop to canvas UV region
                GUI.DrawTextureWithTexCoords(drawRect, tex, new Rect(0, vMax, uMax, -vMax));

                var labelRect = new Rect(drawRect.x + 4, drawRect.y + 4, 300, 20);
                GUI.Label(labelRect, $"Canvas: {canvasW}x{canvasH}  Surface: {surfW}x{surfH}",
                    EditorStyles.miniLabel);
            }
            else
            {
                string hint = DisplayXRPreviewSession.IsRunning
                    ? "Waiting for shared texture..."
                    : "Click 'Start Preview' to begin.";
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

            if (hasInfo)
            {
                // Enumerate modes once session is running and display info is available
                EnumerateRenderingModesIfNeeded();

                EditorGUILayout.BeginHorizontal(EditorStyles.toolbar);
                string modeName = (m_RenderingModeNames != null &&
                    m_CurrentRenderingMode >= 0 && m_CurrentRenderingMode < m_RenderingModeNames.Length)
                    ? m_RenderingModeNames[m_CurrentRenderingMode]
                    : (m_CurrentRenderingMode <= 0 ? "2D" : "3D");
                int modeCount = m_RenderingModeNames != null ? m_RenderingModeNames.Length : 0;

                // Build tiling info string for current mode
                string tileInfo = "";
                if (m_ModeTileColumns != null && m_CurrentRenderingMode >= 0 &&
                    m_CurrentRenderingMode < m_ModeTileColumns.Length)
                {
                    uint vc = m_ModeViewCounts[m_CurrentRenderingMode];
                    uint tc = m_ModeTileColumns[m_CurrentRenderingMode];
                    uint tr = m_ModeTileRows[m_CurrentRenderingMode];
                    uint vw = m_ModeViewWidths[m_CurrentRenderingMode];
                    uint vh = m_ModeViewHeights[m_CurrentRenderingMode];
                    tileInfo = $"  Views: {vc} ({tc}x{tr} tiles, {vw}x{vh}px)";
                }

                GUILayout.Label($"Display: {info.displayPixelWidth}x{info.displayPixelHeight}  " +
                    $"{info.displayWidthMeters * 100:F1}x{info.displayHeightMeters * 100:F1}cm  " +
                    $"Tracked: {(tracked ? "Yes" : "No")}  " +
                    $"Mode: {modeName}{tileInfo}",
                    EditorStyles.miniLabel);
                string hintText = modeCount > 1
                    ? $"Display modes [0-{modeCount - 1}] | V to Cycle"
                    : "V to Cycle";
                GUILayout.Label(hintText, EditorStyles.miniLabel);
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

        // Rendering mode metadata (parallel arrays from enumerate)
        private uint[] m_ModeViewCounts;
        private uint[] m_ModeTileColumns;
        private uint[] m_ModeTileRows;
        private uint[] m_ModeViewWidths;
        private uint[] m_ModeViewHeights;

        private void EnumerateRenderingModesIfNeeded()
        {
            if (m_RenderingModeNames != null) return;

            DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                0, out uint count, null, IntPtr.Zero,
                null, null, null, null, null, null, null, null);
            if (count == 0) return;

            uint[] indices = new uint[count];
            m_RenderingModeNames = new string[count];
            m_ModeViewCounts = new uint[count];
            m_ModeTileColumns = new uint[count];
            m_ModeTileRows = new uint[count];
            m_ModeViewWidths = new uint[count];
            m_ModeViewHeights = new uint[count];
            float[] scaleX = new float[count];
            float[] scaleY = new float[count];
            int[] hw3d = new int[count];

            // Allocate unmanaged buffer for mode names (256 bytes each)
            IntPtr namesPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)(count * 256));
            try
            {
                DisplayXRNative.displayxr_standalone_enumerate_rendering_modes(
                    count, out uint fetched, indices, namesPtr,
                    m_ModeViewCounts, m_ModeTileColumns, m_ModeTileRows,
                    m_ModeViewWidths, m_ModeViewHeights,
                    scaleX, scaleY, hw3d);
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
            if (!DisplayXRPreviewSession.IsRunning || !DisplayXRPreviewSession.SharedTextureAvailable)
                return null;

            return UpdateSharedTexture();
        }

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
