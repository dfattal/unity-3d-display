// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using System;
using UnityEditor;
using UnityEngine;
using Monado.Display3D;

namespace Monado.Display3D.Editor
{
    /// <summary>
    /// XR Plugin Management settings page for Monado 3D Display.
    /// Appears in Project Settings > XR Plug-in Management > OpenXR > Monado 3D Display.
    /// </summary>
    public class Monado3DSettingsProvider : SettingsProvider
    {
        private const string SettingsPath = "Project/XR Plug-in Management/OpenXR/Monado 3D Display";

        public Monado3DSettingsProvider()
            : base(SettingsPath, SettingsScope.Project)
        {
            keywords = new System.Collections.Generic.HashSet<string>(new[]
            {
                "monado", "3d", "display", "openxr", "stereo", "light field"
            });
        }

        [SettingsProvider]
        public static SettingsProvider CreateProvider()
        {
            return new Monado3DSettingsProvider();
        }

        public override void OnGUI(string searchContext)
        {
            EditorGUILayout.LabelField("Monado 3D Display Settings", EditorStyles.boldLabel);
            EditorGUILayout.Space();

            // Runtime status
            DrawRuntimeStatus();

            EditorGUILayout.Space();

            // Environment variable info
            DrawEnvironmentInfo();

            EditorGUILayout.Space();

            // Feature status
            DrawFeatureStatus();
        }

        private void DrawRuntimeStatus()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);

            string runtimeJson = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
            if (!string.IsNullOrEmpty(runtimeJson))
            {
                EditorGUILayout.LabelField("XR_RUNTIME_JSON", runtimeJson);
                bool exists = System.IO.File.Exists(runtimeJson);
                EditorGUILayout.LabelField("File Exists", exists ? "Yes" : "No");
                if (!exists)
                {
                    EditorGUILayout.HelpBox(
                        "The runtime manifest file does not exist. Check the path.",
                        MessageType.Error);
                }
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "XR_RUNTIME_JSON not set. The system default OpenXR runtime will be used.\n" +
                    "Set this environment variable to point to Monado's openxr_monado.json manifest.",
                    MessageType.Warning);
            }

            EditorGUILayout.EndVertical();
        }

        private void DrawEnvironmentInfo()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Display Driver", EditorStyles.boldLabel);

            string simDisplay = Environment.GetEnvironmentVariable("SIM_DISPLAY_ENABLE");
            if (simDisplay == "1")
            {
                string output = Environment.GetEnvironmentVariable("SIM_DISPLAY_OUTPUT") ?? "sbs";
                EditorGUILayout.LabelField("Simulation Display", $"Enabled (output: {output})");
                EditorGUILayout.HelpBox(
                    "Using simulation display driver. No physical 3D display hardware required.",
                    MessageType.Info);
            }
            else
            {
                string srSdk = Environment.GetEnvironmentVariable("LEIASR_SDKROOT");
                if (!string.IsNullOrEmpty(srSdk))
                {
                    EditorGUILayout.LabelField("LeiaSR SDK", srSdk);
                }
                else
                {
                    EditorGUILayout.LabelField("Display Driver", "Not detected");
                    EditorGUILayout.HelpBox(
                        "No display driver detected. Set SIM_DISPLAY_ENABLE=1 for testing " +
                        "or install the display vendor's SR SDK.",
                        MessageType.Info);
                }
            }

            EditorGUILayout.EndVertical();
        }

        private void DrawFeatureStatus()
        {
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Feature Status", EditorStyles.boldLabel);

            if (!Application.isPlaying)
            {
                EditorGUILayout.LabelField("Status", "Not running (enter Play mode to connect)");
                return;
            }

            var feature = Monado3DFeature.Instance;
            if (feature == null)
            {
                EditorGUILayout.HelpBox(
                    "Monado3DFeature is not active. Enable it in:\n" +
                    "Project Settings > XR Plug-in Management > OpenXR > Features",
                    MessageType.Warning);
                return;
            }

            var info = feature.DisplayInfo;
            if (info.isValid)
            {
                EditorGUILayout.LabelField("Connected", "Yes");
                EditorGUILayout.LabelField("Display",
                    $"{info.displayPixelWidth}x{info.displayPixelHeight}");
                EditorGUILayout.LabelField("Physical Size",
                    $"{info.displayWidthMeters * 100:F1} x {info.displayHeightMeters * 100:F1} cm");
                EditorGUILayout.LabelField("Nominal Viewer",
                    $"({info.nominalViewerX * 1000:F0}, {info.nominalViewerY * 1000:F0}, " +
                    $"{info.nominalViewerZ * 1000:F0}) mm");
                EditorGUILayout.LabelField("View Scale",
                    $"{info.recommendedViewScaleX:F2} x {info.recommendedViewScaleY:F2}");
                EditorGUILayout.LabelField("Mode Switch",
                    info.supportsDisplayModeSwitch ? "Supported" : "Not Available");
                EditorGUILayout.LabelField("Eye Tracking",
                    feature.IsEyeTracked ? "Active" : "Inactive");
            }
            else
            {
                EditorGUILayout.LabelField("Connected", "No (display info not available)");
            }

            EditorGUILayout.EndVertical();
        }
    }
}
