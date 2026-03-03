// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using System.Runtime.InteropServices;

namespace Monado.Display3D
{
    /// <summary>
    /// Stereo rig tunable parameters passed to the native plugin.
    /// These modify how xrLocateViews output is transformed before Kooima projection.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Monado3DTunables
    {
        /// <summary>
        /// Scales inter-eye distance. 1.0 = natural, &lt;1 = reduced stereo, &gt;1 = exaggerated.
        /// </summary>
        public float ipdFactor;

        /// <summary>
        /// Scales eye X/Y offset from display center. 1.0 = natural parallax.
        /// </summary>
        public float parallaxFactor;

        /// <summary>
        /// Scales eye Z only (depth intensity without changing baseline). 1.0 = natural.
        /// </summary>
        public float perspectiveFactor;

        /// <summary>
        /// Virtual display size relative to physical. 1.0 = match physical display.
        /// Affects Kooima screen extents.
        /// </summary>
        public float scaleFactor;

        /// <summary>
        /// Override convergence distance in meters. 0 = use nominalViewerZ from display info.
        /// Used by camera-centric mode.
        /// </summary>
        public float convergenceDistance;

        /// <summary>
        /// Override FOV in radians. 0 = compute from display geometry.
        /// Used by camera-centric mode.
        /// </summary>
        public float fovOverride;

        /// <summary>
        /// Whether to use camera-centric mode parameters (convergenceDistance, fovOverride).
        /// </summary>
        [MarshalAs(UnmanagedType.U1)]
        public bool cameraCentricMode;

        /// <summary>Returns default tunables with all factors at 1.0 (natural).</summary>
        public static Monado3DTunables Default => new Monado3DTunables
        {
            ipdFactor = 1.0f,
            parallaxFactor = 1.0f,
            perspectiveFactor = 1.0f,
            scaleFactor = 1.0f,
            convergenceDistance = 0.0f,
            fovOverride = 0.0f,
            cameraCentricMode = false,
        };
    }
}
