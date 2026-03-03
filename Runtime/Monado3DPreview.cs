// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Monado.Display3D
{
    /// <summary>
    /// Editor preview component providing SBS stereo preview without a running runtime,
    /// and optionally offscreen readback preview with the runtime.
    /// </summary>
    [AddComponentMenu("Monado3D/Preview")]
    public class Monado3DPreview : MonoBehaviour
    {
        public enum PreviewMode
        {
            /// <summary>Side-by-side stereo preview computed locally (no runtime).</summary>
            SideBySide,

            /// <summary>Readback from Monado runtime (shows actual display processing).</summary>
            Readback,
        }

        [Header("Preview Settings")]

        [Tooltip("SBS: local stereo preview (no runtime). Readback: actual processed output from runtime.")]
        public PreviewMode mode = PreviewMode.SideBySide;

        [Tooltip("Resolution of the SBS preview texture (total width, both eyes).")]
        public Vector2Int sbsResolution = new Vector2Int(1920, 540);

        /// <summary>The preview texture (SBS or readback). Use in UI or EditorWindow.</summary>
        public Texture2D PreviewTexture { get; private set; }

        /// <summary>Whether readback data is available.</summary>
        public bool ReadbackAvailable { get; private set; }

        private Camera m_LeftCam;
        private Camera m_RightCam;
        private RenderTexture m_LeftRT;
        private RenderTexture m_RightRT;

        void OnEnable()
        {
            int halfW = sbsResolution.x / 2;
            int h = sbsResolution.y;

            PreviewTexture = new Texture2D(sbsResolution.x, h, TextureFormat.RGBA32, false)
            {
                name = "Monado3D_Preview",
                filterMode = FilterMode.Bilinear,
            };

            if (mode == PreviewMode.SideBySide)
            {
                SetupSBSCameras(halfW, h);
            }
        }

        void OnDisable()
        {
            CleanupSBSCameras();

            if (PreviewTexture != null)
            {
                if (Application.isPlaying)
                    Destroy(PreviewTexture);
                else
                    DestroyImmediate(PreviewTexture);
                PreviewTexture = null;
            }
        }

        void LateUpdate()
        {
            if (mode == PreviewMode.Readback)
            {
                UpdateReadback();
            }
        }

        void OnRenderImage(RenderTexture src, RenderTexture dest)
        {
            if (mode == PreviewMode.SideBySide && m_LeftRT != null && m_RightRT != null)
            {
                // Composite SBS: blit left eye to left half, right eye to right half
                int halfW = sbsResolution.x / 2;
                RenderTexture.active = null;

                // Read left eye
                RenderTexture.active = m_LeftRT;
                PreviewTexture.ReadPixels(new Rect(0, 0, halfW, sbsResolution.y), 0, 0, false);

                // Read right eye
                RenderTexture.active = m_RightRT;
                PreviewTexture.ReadPixels(new Rect(0, 0, halfW, sbsResolution.y), halfW, 0, false);

                PreviewTexture.Apply(false);
                RenderTexture.active = null;
            }

            Graphics.Blit(src, dest);
        }

        private void UpdateReadback()
        {
            var feature = Monado3DFeature.Instance;
            if (feature == null) return;

            Monado3DNative.monado3d_get_readback(out IntPtr pixels, out uint w, out uint h, out int ready);

            ReadbackAvailable = ready != 0 && pixels != IntPtr.Zero;

            if (ReadbackAvailable)
            {
                // Resize texture if needed
                if (PreviewTexture.width != (int)w || PreviewTexture.height != (int)h)
                {
                    PreviewTexture.Reinitialize((int)w, (int)h);
                }

                // Copy native pixel data to texture
                PreviewTexture.LoadRawTextureData(pixels, (int)(w * h * 4));
                PreviewTexture.Apply(false);
            }
        }

        private void SetupSBSCameras(int halfW, int h)
        {
            m_LeftRT = new RenderTexture(halfW, h, 24, RenderTextureFormat.ARGB32);
            m_RightRT = new RenderTexture(halfW, h, 24, RenderTextureFormat.ARGB32);
            m_LeftRT.Create();
            m_RightRT.Create();

            // Create left camera
            var leftGO = new GameObject("Monado3D_PreviewLeft");
            leftGO.transform.SetParent(transform, false);
            leftGO.hideFlags = HideFlags.HideAndDontSave;
            m_LeftCam = leftGO.AddComponent<Camera>();
            m_LeftCam.targetTexture = m_LeftRT;
            m_LeftCam.enabled = false; // Render manually

            // Create right camera
            var rightGO = new GameObject("Monado3D_PreviewRight");
            rightGO.transform.SetParent(transform, false);
            rightGO.hideFlags = HideFlags.HideAndDontSave;
            m_RightCam = rightGO.AddComponent<Camera>();
            m_RightCam.targetTexture = m_RightRT;
            m_RightCam.enabled = false;
        }

        private void CleanupSBSCameras()
        {
            void SafeDestroy(UnityEngine.Object obj)
            {
                if (obj == null) return;
                if (Application.isPlaying) Destroy(obj);
                else DestroyImmediate(obj);
            }

            if (m_LeftCam != null) SafeDestroy(m_LeftCam.gameObject);
            if (m_RightCam != null) SafeDestroy(m_RightCam.gameObject);

            if (m_LeftRT != null) { m_LeftRT.Release(); SafeDestroy(m_LeftRT); }
            if (m_RightRT != null) { m_RightRT.Release(); SafeDestroy(m_RightRT); }

            m_LeftCam = null;
            m_RightCam = null;
            m_LeftRT = null;
            m_RightRT = null;
        }
    }
}
