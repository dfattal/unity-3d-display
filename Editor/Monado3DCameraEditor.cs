// Copyright 2024-2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;
using Monado.Display3D;

namespace Monado.Display3D.Editor
{
    [CustomEditor(typeof(Monado3DCamera))]
    public class Monado3DCameraEditor : UnityEditor.Editor
    {
        private SerializedProperty m_IpdFactor;
        private SerializedProperty m_ParallaxFactor;
        private SerializedProperty m_ConvergenceDistance;
        private SerializedProperty m_FieldOfView;
        private SerializedProperty m_LogEyeTracking;

        void OnEnable()
        {
            m_IpdFactor = serializedObject.FindProperty("ipdFactor");
            m_ParallaxFactor = serializedObject.FindProperty("parallaxFactor");
            m_ConvergenceDistance = serializedObject.FindProperty("convergenceDistance");
            m_FieldOfView = serializedObject.FindProperty("fieldOfView");
            m_LogEyeTracking = serializedObject.FindProperty("logEyeTracking");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            EditorGUILayout.HelpBox(
                "Camera-Centric mode works like a standard Unity camera with added stereo. " +
                "You control pose and FOV; convergence distance sets the screen plane depth. " +
                "Best for first-person and free-camera setups.",
                MessageType.Info);

            DrawDisplayInfoBox();

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Stereo Tunables", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_IpdFactor,
                new GUIContent("IPD Factor", "Scales inter-eye distance. 1.0 = natural."));
            EditorGUILayout.PropertyField(m_ParallaxFactor,
                new GUIContent("Parallax Factor", "Scales eye offset from viewing center."));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Camera-Centric Parameters", EditorStyles.boldLabel);

            EditorGUILayout.PropertyField(m_ConvergenceDistance,
                new GUIContent("Convergence Distance",
                    "Distance to virtual screen plane (meters). Auto-set from display info."));

            // Show diopters alongside meters
            float dist = m_ConvergenceDistance.floatValue;
            if (dist > 0.01f)
            {
                float diopters = 1.0f / dist;
                EditorGUI.indentLevel++;
                EditorGUILayout.LabelField(" ", $"{dist:F2} m  ({diopters:F1} dpt)");
                EditorGUI.indentLevel--;
            }

            // Show computed FOV when auto-computing
            if (m_FieldOfView.floatValue <= 0f)
            {
                var feature = Monado3DFeature.Instance;
                if (feature != null && feature.DisplayInfo.isValid)
                {
                    var info = feature.DisplayInfo;
                    float halfW = info.displayWidthMeters * 0.5f;
                    float nz = info.nominalViewerZ > 0.01f ? info.nominalViewerZ : 0.5f;
                    float ratio = m_ConvergenceDistance.floatValue / nz;
                    float virtualHalfW = halfW * ratio;
                    float computedFov = Mathf.Atan2(virtualHalfW, m_ConvergenceDistance.floatValue) * 2f * Mathf.Rad2Deg;
                    EditorGUILayout.LabelField("Computed FOV", $"{computedFov:F1} deg (auto)");
                }
            }

            EditorGUILayout.PropertyField(m_FieldOfView,
                new GUIContent("FOV Override",
                    "Field of view in degrees. 0 = auto-compute from convergence + display."));

            EditorGUILayout.Space();
            EditorGUILayout.PropertyField(m_LogEyeTracking);

            // Reset button
            EditorGUILayout.Space();
            if (GUILayout.Button("Reset to Defaults"))
            {
                m_IpdFactor.floatValue = 1.0f;
                m_ParallaxFactor.floatValue = 1.0f;
                m_ConvergenceDistance.floatValue = 0.5f;
                m_FieldOfView.floatValue = 0f;
            }

            // Runtime info
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
