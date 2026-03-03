// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D
{
    /// <summary>
    /// Camera-centric stereo rig. Attach to an existing Camera.
    /// The camera position is the nominal viewing position; eyes move around it.
    /// Internally converts camera-centric parameters to Kooima tunables.
    /// </summary>
    [AddComponentMenu("Monado3D/Camera-Centric Rig")]
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Camera))]
    public class Monado3DCamera : MonoBehaviour
    {
        [Header("Stereo Tunables")]

        [Tooltip("Scales inter-eye distance. 1.0 = natural, <1 = reduced stereo, >1 = exaggerated.")]
        [Range(0f, 3f)]
        public float ipdFactor = 1.0f;

        [Tooltip("Scales eye X/Y offset. 1.0 = natural parallax.")]
        [Range(0f, 3f)]
        public float parallaxFactor = 1.0f;

        [Header("Camera-Centric Parameters")]

        [Tooltip("Distance to the virtual screen plane in meters. " +
                 "Default: nominalViewerZ from display info (~0.5m).")]
        [Range(0.1f, 5f)]
        public float convergenceDistance = 0.5f;

        [Tooltip("Field of view in degrees. 0 = auto-compute from convergence + display size.")]
        [Range(0f, 120f)]
        public float fieldOfView;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private Monado3DFeature m_Feature;
        private bool m_InitializedConvergence;

        void OnEnable()
        {
            m_Feature = Monado3DFeature.Instance;
        }

        void LateUpdate()
        {
            if (m_Feature == null)
            {
                m_Feature = Monado3DFeature.Instance;
                if (m_Feature == null) return;
            }

            // Auto-set convergence distance from display info on first frame
            if (!m_InitializedConvergence && m_Feature.DisplayInfo.isValid)
            {
                float nz = m_Feature.DisplayInfo.nominalViewerZ;
                if (nz > 0.01f)
                {
                    convergenceDistance = nz;
                }
                m_InitializedConvergence = true;
            }

            // Convert camera-centric params to native tunables
            var tunables = new Monado3DTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = 1.0f, // Not used in camera-centric mode
                scaleFactor = 1.0f,       // Not used in camera-centric mode
                convergenceDistance = convergenceDistance,
                fovOverride = fieldOfView > 0f ? fieldOfView * Mathf.Deg2Rad : 0f,
                cameraCentricMode = true,
            };

            m_Feature.SetTunables(tunables);
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
            // Draw virtual screen plane at convergence distance
            var info = Monado3DFeature.Instance?.DisplayInfo ?? default;

            float aspect = info.isValid
                ? info.displayWidthMeters / info.displayHeightMeters
                : 16f / 9f;

            float halfH;
            if (fieldOfView > 0f)
            {
                halfH = convergenceDistance * Mathf.Tan(fieldOfView * 0.5f * Mathf.Deg2Rad);
            }
            else if (info.isValid && info.nominalViewerZ > 0.01f)
            {
                float ratio = convergenceDistance / info.nominalViewerZ;
                halfH = info.displayHeightMeters * ratio * 0.5f;
            }
            else
            {
                halfH = convergenceDistance * Mathf.Tan(30f * Mathf.Deg2Rad);
            }

            float w = halfH * 2f * aspect;
            float h = halfH * 2f;

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.3f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Vector3 center = new Vector3(0, 0, convergenceDistance);
            Gizmos.DrawCube(center, new Vector3(w, h, 0.002f));

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.8f);
            Gizmos.DrawWireCube(center, new Vector3(w, h, 0.002f));
        }
#endif
    }
}
