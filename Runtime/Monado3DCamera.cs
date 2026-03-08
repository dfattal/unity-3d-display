// Copyright 2024-2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D
{
    /// <summary>
    /// Camera-centric stereo rig. Attach to a Camera whose transform represents the
    /// viewer pose. The camera's vertical FOV is inherited as the rendering FOV.
    /// Exposes only: IPD, parallax, and inverse convergence distance.
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

        [Header("Convergence")]

        [Tooltip("Inverse convergence distance (1/meters). 0 = infinity (parallel projection). " +
                 "Higher values = screen plane closer to camera.")]
        [Range(0f, 10f)]
        public float invConvergenceDistance = 0f;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private Monado3DFeature m_Feature;
        private float m_CachedCameraFov;
        private Camera m_Camera;
        private bool m_InitializedInvd;

        void OnEnable()
        {
            m_Feature = Monado3DFeature.Instance;
            m_Camera = GetComponent<Camera>();
            // Cache the camera's FOV BEFORE XR overrides it. Once XR is active,
            // Camera.fieldOfView returns the Kooima FOV we set, creating a
            // feedback loop that collapses the FOV to zero.
            m_CachedCameraFov = m_Camera.fieldOfView;
            // Guard against XR having already overridden the FOV to near-zero
            if (m_CachedCameraFov < 1.0f)
                m_CachedCameraFov = 60.0f;
        }

        void OnDisable()
        {
            Debug.Log("[Monado3D] Monado3DCamera.OnDisable");
            m_Feature = null;
        }

        void LateUpdate()
        {
            if (m_Feature == null)
            {
                m_Feature = Monado3DFeature.Instance;
                if (m_Feature == null) return;
            }

            // Auto-init invConvergenceDistance from display info on first frame.
            if (!m_InitializedInvd)
            {
                var info = m_Feature.DisplayInfo;
                if (info.isValid && info.nominalViewerZ > 0f)
                {
                    invConvergenceDistance = 1.0f / info.nominalViewerZ;
                    m_InitializedInvd = true;
                }
            }

            // Compute half_tan_vfov from the cached camera FOV
            float halfTanVfov = Mathf.Tan(m_CachedCameraFov * 0.5f * Mathf.Deg2Rad);

            // Convert camera-centric params to native tunables
            var tunables = new Monado3DTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = 1.0f,
                virtualDisplayHeight = 0f,
                invConvergenceDistance = invConvergenceDistance,
                fovOverride = halfTanVfov,
                cameraCentricMode = true,
            };

            m_Feature.SetTunables(tunables);

            // Send camera world pose to native
            m_Feature.SetSceneTransform(
                transform.position,
                transform.rotation,
                Vector3.one,
                enabled: true);

            m_Feature.RefreshEyePositions();

            if (logEyeTracking)
            {
                Debug.Log($"[Monado3D] Eyes: L={m_Feature.LeftEyePosition}, " +
                          $"R={m_Feature.RightEyePosition}, tracked={m_Feature.IsEyeTracked}" +
                          $" camPos={transform.position} fov={m_CachedCameraFov:F1}" +
                          $" invd={invConvergenceDistance:F4}");
            }
        }

#if UNITY_EDITOR
        void OnDrawGizmosSelected()
        {
            // Draw virtual screen plane at convergence distance (if finite)
            if (invConvergenceDistance < 0.001f) return;

            float convergenceDist = 1.0f / invConvergenceDistance;
            var info = Monado3DFeature.Instance?.DisplayInfo ?? default;

            float aspect = info.isValid
                ? info.displayWidthMeters / info.displayHeightMeters
                : 16f / 9f;

            float fovDeg = m_CachedCameraFov > 0f ? m_CachedCameraFov
                         : GetComponent<Camera>().fieldOfView;
            float halfH = convergenceDist * Mathf.Tan(fovDeg * 0.5f * Mathf.Deg2Rad);

            float w = halfH * 2f * aspect;
            float h = halfH * 2f;

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.3f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Vector3 center = new Vector3(0, 0, convergenceDist);
            Gizmos.DrawCube(center, new Vector3(w, h, 0.002f));

            Gizmos.color = new Color(1f, 0.6f, 0.2f, 0.8f);
            Gizmos.DrawWireCube(center, new Vector3(w, h, 0.002f));
        }
#endif
    }
}
