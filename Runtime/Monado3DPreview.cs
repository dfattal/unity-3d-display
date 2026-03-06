// Copyright 2024-2026, Monado 3D Display contributors
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

            /// <summary>Zero-copy shared GPU texture from runtime (fastest, macOS only for now).</summary>
            SharedTexture,
        }

        [Header("Preview Settings")]

        [Tooltip("SBS: local stereo preview (no runtime). Readback: CPU readback from runtime. SharedTexture: zero-copy GPU preview (macOS).")]
        public PreviewMode mode = PreviewMode.SideBySide;

        [Tooltip("Resolution of the SBS preview texture (total width, both eyes).")]
        public Vector2Int sbsResolution = new Vector2Int(1920, 540);

        /// <summary>The preview texture (SBS or readback). Use in UI or EditorWindow.</summary>
        public Texture2D PreviewTexture { get; private set; }

        /// <summary>Whether readback data is available.</summary>
        public bool ReadbackAvailable { get; private set; }

        /// <summary>Whether shared texture data is available.</summary>
        public bool SharedTextureAvailable { get; private set; }

        private Texture2D m_SharedTexture;
        private IntPtr m_SharedNativePtr;
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

            // Only create SBS child cameras when XR is NOT active.
            // SBS mode is for offline preview; when XR is running, the child
            // cameras (parented to the XR camera) trigger mirror blit artifacts.
            if (mode == PreviewMode.SideBySide && !UnityEngine.XR.XRSettings.isDeviceActive)
            {
                SetupSBSCameras(halfW, h);
            }
        }

        void OnDisable()
        {
            Debug.Log("[Monado3D] Monado3DPreview.OnDisable BEGIN");
            try
            {
                CleanupSBSCameras();
                CleanupSharedTexture();

                if (PreviewTexture != null)
                {
                    if (Application.isPlaying)
                        Destroy(PreviewTexture);
                    else
                        DestroyImmediate(PreviewTexture);
                    PreviewTexture = null;
                }
            }
            catch (System.Exception e)
            {
                Debug.LogWarning($"[Monado3D] Preview cleanup exception: {e.Message}");
            }
            Debug.Log("[Monado3D] Monado3DPreview.OnDisable END");
        }

        void LateUpdate()
        {
            switch (mode)
            {
                case PreviewMode.Readback:
                    UpdateReadback();
                    break;
                case PreviewMode.SharedTexture:
                    UpdateSharedTexture();
                    break;
                case PreviewMode.SideBySide:
                    UpdateSideBySide();
                    break;
            }
        }

        // NOTE: Do NOT use OnRenderImage here. Having that callback on an XR-
        // controlled camera forces Unity to insert an intermediate render target
        // and extra blit after each eye render, causing SBS flicker artifacts
        // on editor repaints (mouse moves, GUI events, etc.).

        private void UpdateSideBySide()
        {
            // SBS mode is for offline stereo preview without a running XR session.
            // When XR is active, Camera.Render() on child cameras triggers mirror
            // blit artifacts that bleed into editor window repaints.
            if (UnityEngine.XR.XRSettings.isDeviceActive)
                return;

            if (m_LeftCam == null || m_RightCam == null ||
                m_LeftRT == null || m_RightRT == null)
                return;

            // Copy settings from parent camera
            var parentCam = GetComponent<Camera>();
            if (parentCam != null)
            {
                m_LeftCam.CopyFrom(parentCam);
                m_LeftCam.targetTexture = m_LeftRT;
                m_LeftCam.enabled = false;

                m_RightCam.CopyFrom(parentCam);
                m_RightCam.targetTexture = m_RightRT;
                m_RightCam.enabled = false;
            }

            // Render both eyes manually (off the XR pipeline)
            m_LeftCam.Render();
            m_RightCam.Render();

            // Composite into SBS preview texture
            int halfW = sbsResolution.x / 2;

            RenderTexture.active = m_LeftRT;
            PreviewTexture.ReadPixels(new Rect(0, 0, halfW, sbsResolution.y), 0, 0, false);

            RenderTexture.active = m_RightRT;
            PreviewTexture.ReadPixels(new Rect(0, 0, halfW, sbsResolution.y), halfW, 0, false);

            PreviewTexture.Apply(false);
            RenderTexture.active = null;
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

        private void UpdateSharedTexture()
        {
            var feature = Monado3DFeature.Instance;
            if (feature == null || !feature.SharedTextureAvailable) return;

            Monado3DNative.monado3d_get_shared_texture(
                out IntPtr nativePtr, out uint w, out uint h, out int ready);

            SharedTextureAvailable = ready != 0 && nativePtr != IntPtr.Zero;

            if (!SharedTextureAvailable)
            {
                // Fall through to readback as fallback
                UpdateReadback();
                return;
            }

            if (m_SharedTexture == null || m_SharedNativePtr != nativePtr ||
                m_SharedTexture.width != (int)w || m_SharedTexture.height != (int)h)
            {
                // First frame or size/pointer change: create external texture
                if (m_SharedTexture != null)
                {
                    if (Application.isPlaying) Destroy(m_SharedTexture);
                    else DestroyImmediate(m_SharedTexture);
                }

                m_SharedTexture = Texture2D.CreateExternalTexture(
                    (int)w, (int)h, TextureFormat.BGRA32, false, false, nativePtr);
                m_SharedTexture.name = "Monado3D_SharedPreview";
                m_SharedTexture.filterMode = FilterMode.Bilinear;
                m_SharedNativePtr = nativePtr;
            }
            else
            {
                // Subsequent frames: update to pick up new content
                m_SharedTexture.UpdateExternalTexture(nativePtr);
            }

            PreviewTexture = m_SharedTexture;
        }

        private void CleanupSharedTexture()
        {
            if (m_SharedTexture != null)
            {
                if (Application.isPlaying) Destroy(m_SharedTexture);
                else DestroyImmediate(m_SharedTexture);
                m_SharedTexture = null;
            }
            m_SharedNativePtr = IntPtr.Zero;
            SharedTextureAvailable = false;

            // Don't call native destroy here — Monado3DFeature.OnSessionDestroy
            // already handles it. Double-destroy would CFRelease a freed IOSurface.
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
