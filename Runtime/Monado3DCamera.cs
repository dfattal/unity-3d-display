// Copyright 2024-2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
using UnityEngine.Rendering;

namespace Monado.Display3D
{
    /// <summary>
    /// Camera-centric stereo rig. Attach to a Camera whose transform represents the
    /// viewer pose. The camera's vertical FOV is inherited as the rendering FOV.
    /// Adds convergence distance (screen plane depth), IPD, and parallax controls.
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
        private Camera m_Camera;

        private bool m_Disabled;
        private bool m_MatricesApplied;

        void OnEnable()
        {
            m_Feature = Monado3DFeature.Instance;
            m_Camera = GetComponent<Camera>();
            m_Disabled = false;
            // Cache the camera's FOV BEFORE XR overrides it. Once XR is active,
            // Camera.fieldOfView returns the Kooima FOV we set, creating a
            // feedback loop that collapses the FOV to zero.
            m_CachedCameraFov = m_Camera.fieldOfView;
            // Guard against XR having already overridden the FOV to near-zero
            if (m_CachedCameraFov < 1.0f)
                m_CachedCameraFov = 60.0f;

            RenderPipelineManager.beginCameraRendering += OnBeginCameraRendering;
        }

        void OnDisable()
        {
            Debug.Log("[Monado3D] Monado3DCamera.OnDisable");
            RenderPipelineManager.beginCameraRendering -= OnBeginCameraRendering;
            if (m_MatricesApplied && m_Camera != null)
            {
                m_Camera.ResetStereoViewMatrices();
                m_Camera.ResetStereoProjectionMatrices();
                m_MatricesApplied = false;
            }
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

            // Auto-init convergenceDistance from display info on first frame.
            if (!m_InitializedConvergence)
            {
                var info = m_Feature.DisplayInfo;
                if (info.isValid && info.nominalViewerZ > 0f)
                {
                    convergenceDistance = info.nominalViewerZ;
                    m_InitializedConvergence = true;
                }
            }

            // When fieldOfView == 0, compute FOV from the physical display geometry.
            // The virtual screen must match the physical display's angular extent
            // so the compositor maps the Kooima projection correctly.
            float fov;
            if (fieldOfView > 0f)
            {
                fov = fieldOfView;
            }
            else
            {
                var info = m_Feature.DisplayInfo;
                if (info.isValid && info.displayHeightMeters > 0f && info.nominalViewerZ > 0f)
                    fov = 2f * Mathf.Atan(info.displayHeightMeters / (2f * info.nominalViewerZ)) * Mathf.Rad2Deg;
                else
                    fov = m_CachedCameraFov;
            }

            // Convert camera-centric params to native tunables
            var tunables = new Monado3DTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = 1.0f, // Not used in camera-centric mode
                virtualDisplayHeight = 0f, // Not used in camera-centric mode
                convergenceDistance = convergenceDistance,
                fovOverride = fov * Mathf.Deg2Rad,
                cameraCentricMode = true,
            };

            m_Feature.SetTunables(tunables);

            // Send camera world pose to native — the Kooima library needs the
            // camera pose to place the virtual screen at convergence distance
            // along the camera's forward axis (matching the native test app's cameraPose).
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
                          $" camPos={transform.position} camLocalPos={transform.localPosition}" +
                          $" parentPos={transform.parent?.position}");
            }
        }

        /// <summary>
        /// Apply the Kooima library's matched view+projection matrices directly,
        /// bypassing Unity's matrix reconstruction from (fov, position, orientation).
        /// </summary>
        // Built-in render pipeline callback (URP uses beginCameraRendering instead)
        void OnPreRender()
        {
            ApplyStereoMatrices("OnPreRender");
        }

        void OnBeginCameraRendering(ScriptableRenderContext ctx, Camera cam)
        {
            if (cam != m_Camera) return;
            ApplyStereoMatrices("beginCameraRendering");
        }

        void ApplyStereoMatrices(string source)
        {
            if (m_Feature == null) return;

            if (!m_Feature.GetStereoMatrices(
                    out Matrix4x4 lv, out Matrix4x4 lp,
                    out Matrix4x4 rv, out Matrix4x4 rp))
                return;

            // View matrices: use directly — Unity's worldToCameraMatrix uses OpenGL
            // convention (right-hand, -Z forward), same as the library output.
            Matrix4x4 leftView = lv;
            Matrix4x4 rightView = rv;

            // Projection: pass raw OpenGL convention to SetStereoProjectionMatrix.
            // Unity internally applies GL.GetGPUProjectionMatrix for the platform.
            // Applying it here would double-flip Y on Metal.
            Matrix4x4 leftProj = lp;
            Matrix4x4 rightProj = rp;

            // TEST: skip matrix override to verify if Unity XR ignores SetStereo* calls
            // m_Camera.SetStereoViewMatrix(Camera.StereoscopicEye.Left, leftView);
            // m_Camera.SetStereoViewMatrix(Camera.StereoscopicEye.Right, rightView);
            // m_Camera.SetStereoProjectionMatrix(Camera.StereoscopicEye.Left, leftProj);
            // m_Camera.SetStereoProjectionMatrix(Camera.StereoscopicEye.Right, rightProj);
            // m_MatricesApplied = true;

            if (logEyeTracking)
            {
                Debug.Log($"[Monado3D] {source}: lib leftProj: " +
                          $"m11={lp.m11:F4} m12={lp.m12:F4} m32={lp.m32:F4}");
                Debug.Log($"[Monado3D] {source}: lib leftView: " +
                          $"m03={lv.m03:F4} m13={lv.m13:F4} m23={lv.m23:F4}");
                var actualLP = m_Camera.GetStereoProjectionMatrix(Camera.StereoscopicEye.Left);
                Debug.Log($"[Monado3D] {source}: ACTUAL proj m11={actualLP.m11:F4} m12={actualLP.m12:F4} m32={actualLP.m32:F4}");
            }
        }

        /// <summary>
        /// Convert view matrix from OpenXR (right-hand) to Unity (left-hand).
        /// Applies S * M * S where S = diag(1, 1, -1, 1).
        /// Elements negated: m02, m12, m32 (column 2 except m22) and m20, m21, m23 (row 2 except m22).
        /// m22 is negated twice (row + column) so stays unchanged.
        /// </summary>
        static Matrix4x4 ConvertViewMatrix(Matrix4x4 m)
        {
            m.m02 = -m.m02; m.m12 = -m.m12; m.m32 = -m.m32; // column 2 (skip m22)
            m.m20 = -m.m20; m.m21 = -m.m21; m.m23 = -m.m23; // row 2 (skip m22)
            return m;
        }

        /// <summary>
        /// Convert projection matrix from OpenGL (right-hand, Z [-1,1]) to Unity (left-hand).
        /// Negates row 2 to flip Z direction for left-hand clip space.
        /// GL.GetGPUProjectionMatrix then handles the Z range remapping.
        /// </summary>
        static Matrix4x4 ConvertProjectionMatrix(Matrix4x4 m)
        {
            // Negate row 2 to convert from right-hand to left-hand clip space
            m.m20 = -m.m20; m.m21 = -m.m21; m.m22 = -m.m22; m.m23 = -m.m23;
            return m;
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
