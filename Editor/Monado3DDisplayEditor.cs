// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using Monado.Display3D;

namespace Monado.Display3D.Editor
{
    [CustomEditor(typeof(Monado3DDisplay))]
    public class Monado3DDisplayEditor : UnityEditor.Editor
    {
        private SerializedProperty m_IpdFactor;
        private SerializedProperty m_ParallaxFactor;
        private SerializedProperty m_PerspectiveFactor;
        private SerializedProperty m_ScaleFactor;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_PerspectiveFactor = serializedObject.FindProperty("perspectiveFactor");
            m_ScaleFactor = serializedObject.FindProperty("scaleFactor");
            m_LogEyeTracking = serializedObject.FindProperty("logEyeTracking");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            // Display info header
            DrawDisplayInfoBox();

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Stereo Tunables", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_IpdFactor,
                new GUIContent("IPD Factor", "Scales inter-eye distance. 1.0 = natural."));
            EditorGUILayout.PropertyField(m_ParallaxFactor,
                new GUIContent("Parallax Factor", "Scales eye X/Y offset from display center."));
            EditorGUILayout.PropertyField(m_PerspectiveFactor,
                new GUIContent("Perspective Factor", "Scales eye Z (depth intensity)."));
            EditorGUILayout.PropertyField(m_ScaleFactor,
                new GUIContent("Scale Factor", "Virtual display size relative to physical."));

            EditorGUILayout.Space();
            EditorGUILayout.PropertyField(m_LogEyeTracking);

            // Reset button
            EditorGUILayout.Space();
            if (GUILayout.Button("Reset to Defaults"))
            {
                m_IpdFactor.floatValue = 1.0f;
                m_ParallaxFactor.floatValue = 1.0f;
                m_PerspectiveFactor.floatValue = 1.0f;
                m_ScaleFactor.floatValue = 1.0f;
            }

            // Runtime eye tracking info
            if (Application.isPlaying && Monado3DFeature.Instance != null)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Runtime Status", EditorStyles.boldLabel);

                var feature = Monado3DFeature.Instance;
                EditorGUILayout.LabelField("Eye Tracked", feature.IsEyeTracked ? "Yes" : "No");
                EditorGUILayout.Vector3Field("Left Eye", feature.LeftEyePosition);
                EditorGUILayout.Vector3Field("Right Eye", feature.RightEyePosition);
            }

            serializedObject.ApplyModifiedProperties();
        }

        private void DrawDisplayInfoBox()
        {
            var feature = Monado3DFeature.Instance;
            if (feature == null || !feature.DisplayInfo.isValid)
            {
                EditorGUILayout.HelpBox(
                    "Display info not available. Monado3DFeature must be active with a connected runtime.",
                    MessageType.Info);
                return;
            }

            var info = feature.DisplayInfo;
            EditorGUILayout.BeginVertical(EditorStyles.helpBox);
            EditorGUILayout.LabelField("Connected Display", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("Resolution", $"{info.displayPixelWidth} x {info.displayPixelHeight}");
            EditorGUILayout.LabelField("Physical Size",
                $"{info.displayWidthMeters * 100:F1} x {info.displayHeightMeters * 100:F1} cm");
            EditorGUILayout.LabelField("Nominal Viewer",
                $"({info.nominalViewerX * 1000:F0}, {info.nominalViewerY * 1000:F0}, {info.nominalViewerZ * 1000:F0}) mm");
            EditorGUILayout.LabelField("Mode Switch", info.supportsDisplayModeSwitch ? "Supported" : "N/A");
            EditorGUILayout.EndVertical();
        }
    }
}
