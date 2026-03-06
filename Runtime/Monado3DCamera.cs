// Copyright 2024-2026, Monado 3D Display contributors
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
        private float m_CachedCameraFov;

        private bool m_Disabled;

        void OnEnable()
        {
            m_Feature = Monado3DFeature.Instance;
            m_Disabled = false;
            // Cache the camera's FOV BEFORE XR overrides it. Once XR is active,
            // Camera.fieldOfView returns the Kooima FOV we set, creating a
            // feedback loop that collapses the FOV to zero.
            m_CachedCameraFov = GetComponent<Camera>().fieldOfView;
            // Guard against XR having already overridden the FOV to near-zero
            if (m_CachedCameraFov < 1.0f)
                m_CachedCameraFov = 60.0f;
        }

        void OnDisable()
        {
            Debug.Log("[Monado3D] Monado3DCamera.OnDisable");
            m_Disabled = true;
            m_Feature = null;
        }

        void LateUpdate()
        {
            if (m_Disabled) return;

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

            // When fieldOfView == 0, use the cached camera FOV from before XR
            // took over. This keeps the Kooima virtual screen size stable and
            // ensures convergence only moves the stereo depth plane.
            float fov = fieldOfView > 0f ? fieldOfView : m_CachedCameraFov;

            // Convert camera-centric params to native tunables
            var tunables = new Monado3DTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = 1.0f, // Not used in camera-centric mode
                scaleFactor = 1.0f,       // Not used in camera-centric mode
                convergenceDistance = convergenceDistance,
                fovOverride = fov * Mathf.Deg2Rad,
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

            float fovDeg = fieldOfView > 0f ? fieldOfView
                         : m_CachedCameraFov > 0f ? m_CachedCameraFov
                         : GetComponent<Camera>().fieldOfView;
            float halfH = convergenceDistance * Mathf.Tan(fovDeg * 0.5f * Mathf.Deg2Rad);

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
