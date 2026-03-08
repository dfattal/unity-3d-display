// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace DisplayXR
{
    /// <summary>
    /// Display-centric stereo rig. Attach to a Camera whose parent transform represents
    /// the virtual display pose (position and orientation of the display surface).
    /// The camera's FOV is ignored — the display's physical geometry and virtualDisplayHeight
    /// determine the frustum. Eyes move around the display based on tracking.
    /// </summary>
    [AddComponentMenu("DisplayXR/Display-Centric Rig")]
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Camera))]
    public class DisplayXRDisplay : MonoBehaviour
    {
        [Header("Stereo Tunables")]

        [Tooltip("Scales inter-eye distance. 1.0 = natural, <1 = reduced stereo, >1 = exaggerated.")]
        [Range(0f, 3f)]
        public float ipdFactor = 1.0f;

        [Tooltip("Scales eye X/Y offset from display center. 1.0 = natural parallax.")]
        [Range(0f, 3f)]
        public float parallaxFactor = 1.0f;

        [Tooltip("Scales perceived depth. 1.0 = natural perspective.")]
        [Range(0f, 3f)]
        public float perspectiveFactor = 1.0f;

        [Tooltip("Virtual display height in meters. 0 = use physical display height.")]
        public float virtualDisplayHeight = 0f;

        [Header("Debug")]

        [Tooltip("Show eye tracking status in the console.")]
        public bool logEyeTracking;

        private DisplayXRFeature m_Feature;
        private Camera m_Camera;

        void OnEnable()
        {
            m_Camera = GetComponent<Camera>();
            m_Feature = DisplayXRFeature.Instance;
            if (m_Feature == null)
            {
                Debug.LogWarning("[DisplayXR] DisplayXRFeature not active. " +
                    "Enable it in Project Settings > XR Plug-in Management > OpenXR.");
            }
        }

        void LateUpdate()
        {
            if (m_Feature == null)
            {
                m_Feature = DisplayXRFeature.Instance;
                if (m_Feature == null) return;
            }

            // Resolve virtualDisplayHeight: 0 means use physical display height
            float vdh = virtualDisplayHeight;
            if (vdh <= 0f && m_Feature.DisplayInfo.isValid)
            {
                vdh = m_Feature.DisplayInfo.displayHeightMeters;
            }

            // Push tunables to native plugin — affects next xrLocateViews
            var tunables = new DisplayXRTunables
            {
                ipdFactor = ipdFactor,
                parallaxFactor = parallaxFactor,
                perspectiveFactor = perspectiveFactor,
                virtualDisplayHeight = vdh,
                invConvergenceDistance = 0f,
                fovOverride = 0f,
                nearZ = m_Camera.nearClipPlane,
                farZ = m_Camera.farClipPlane,
                cameraCentricMode = false,
            };

            m_Feature.SetTunables(tunables);

            // Push scene transform: parent camera's world pose is the display pose.
            // Transform scale acts as zoom: scale > 1 zooms in (display appears bigger).
            m_Feature.SetSceneTransform(
                transform.position,
                transform.rotation,
                transform.lossyScale,
                enabled: true);

            // Refresh eye positions for debug/UI
            m_Feature.RefreshEyePositions();

            if (logEyeTracking)
            {
                Debug.Log($"[DisplayXR] Display: pos={transform.position} " +
                          $"near={m_Camera.nearClipPlane} far={m_Camera.farClipPlane} " +
                          $"camWorldPos={m_Camera.transform.position} " +
                          $"Eyes: L={m_Feature.LeftEyePosition}, " +
                          $"R={m_Feature.RightEyePosition}, tracked={m_Feature.IsEyeTracked}");
            }
        }

#if UNITY_EDITOR
        void OnDrawGizmosSelected()
        {
            // Draw display plane in editor
            var info = DisplayXRFeature.Instance?.DisplayInfo ?? default;
            float h = virtualDisplayHeight > 0 ? virtualDisplayHeight
                     : (info.isValid ? info.displayHeightMeters : 0.2f);
            float w = info.isValid
                ? info.displayWidthMeters * (h / info.displayHeightMeters)
                : h * 1.5f;

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
