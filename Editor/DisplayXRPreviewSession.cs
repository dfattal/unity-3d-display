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
            StopPolling();
            DisplayXRNative.displayxr_standalone_stop();
            SharedTextureAvailable = false;
            DisplayInfo = default;
            Debug.Log("[DisplayXR-SA] Standalone session stopped");
        }

        private static void StartPolling()
        {
            if (s_Polling) return;
            s_Polling = true;
            EditorApplication.update += Poll;
        }

        private static void StopPolling()
        {
            if (!s_Polling) return;
            s_Polling = false;
            EditorApplication.update -= Poll;
        }

        private static void Poll()
        {
            if (!IsRunning)
            {
                StopPolling();
                return;
            }

            DisplayXRNative.displayxr_standalone_poll();
            RefreshEyePositions();
            RefreshSharedTexture();
        }

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
