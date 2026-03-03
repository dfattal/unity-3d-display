// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D
{
    /// <summary>
    /// Display-centric stereo rig. Place at the virtual display position in your scene.
    /// The display is the fixed anchor; eyes move around it based on tracking.
    /// </summary>
    [AddComponentMenu("Monado3D/Display-Centric Rig")]
    [DisallowMultipleComponent]
    public class Monado3DDisplay : MonoBehaviour
    {
        [Header("Stereo Tunables")]

        [Tooltip("Scales inter-eye distance. 1.0 = natural, <1 = reduced stereo, >1 = exaggerated.")]
        [Range(0f, 3f)]
        public float ipdFactor = 1.0f;

        [Tooltip("Scales eye X/Y offset from display center. 1.0 = natural parallax.")]
        [Range(0f, 3f)]
        public float parallaxFactor = 1.0f;

        [Tooltip("Scales eye Z only (depth intensity without changing baseline). 1.0 = natural.")]
        [Range(0f, 3f)]
        public float perspectiveFactor = 1.0f;

        [Tooltip("Virtual display size relative to physical. 1.0 = match physical display.")]
        [Range(0.1f, 5f)]
        public float scaleFactor = 1.0f;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private Monado3DFeature m_Feature;

        void OnEnable()
        {
            m_Feature = Monado3DFeature.Instance;
            if (m_Feature == null)
            {
                Debug.LogWarning("[Monado3D] Monado3DFeature not active. " +
                    "Enable it in Project Settings > XR Plug-in Management > OpenXR.");
            }
        }

        void LateUpdate()
        {
            if (m_Feature == null)
            {
                m_Feature = Monado3DFeature.Instance;
                if (m_Feature == null) return;
            }

            // Push tunables to native plugin — affects next xrLocateViews
            var tunables = new Monado3DTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = perspectiveFactor,
                scaleFactor = scaleFactor,
                convergenceDistance = 0f,
                fovOverride = 0f,
                cameraCentricMode = false,
            };

            m_Feature.SetTunables(tunables);

            // Push scene transform: this component's world pose maps the virtual display
            // into the scene. The scene transform tells the native plugin how to position
            // the raw DISPLAY-space eye positions relative to this virtual display.
            m_Feature.SetSceneTransform(
                transform.position,
                transform.rotation,
                zoomScale: 1.0f,
                enabled: true);

            // Refresh eye positions for debug/UI
            m_Feature.RefreshEyePositions();

            if (logEyeTracking)
            {
                Debug.Log($"[Monado3D] Eyes: L={m_Feature.LeftEyePosition}, " +
                          $"R={m_Feature.RightEyePosition}, tracked={m_Feature.IsEyeTracked}");
            }
        }

#if UNITY_EDITOR
        void OnDrawGizmosSelected()
        {
            // Draw display plane in editor
            var info = Monado3DFeature.Instance?.DisplayInfo ?? default;
            float w = info.isValid ? info.displayWidthMeters * scaleFactor : 0.3f * scaleFactor;
            float h = info.isValid ? info.displayHeightMeters * scaleFactor : 0.2f * scaleFactor;

            Gizmos.color = new Color(0.2f, 0.8f, 1.0f, 0.3f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Gizmos.DrawCube(Vector3.zero, new Vector3(w, h, 0.002f));

            Gizmos.color = new Color(0.2f, 0.8f, 1.0f, 0.8f);
            Gizmos.DrawWireCube(Vector3.zero, new Vector3(w, h, 0.002f));

            // Draw nominal viewer position
            float nz = info.isValid ? info.nominalViewerZ : 0.5f;
            Gizmos.color = Color.yellow;
            Gizmos.DrawSphere(new Vector3(0, 0, nz), 0.01f);
        }
#endif
    }
}
