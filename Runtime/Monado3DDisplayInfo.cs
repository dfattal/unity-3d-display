// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

using System.Runtime.InteropServices;

namespace Monado.Display3D
{
    /// <summary>
    /// Physical display properties queried from the Monado runtime via XR_EXT_display_info.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Monado3DDisplayInfo
    {
        /// <summary>Physical display width in meters.</summary>
        public float displayWidthMeters;

        /// <summary>Physical display height in meters.</summary>
        public float displayHeightMeters;

        /// <summary>Display pixel width.</summary>
        public uint displayPixelWidth;

        /// <summary>Display pixel height.</summary>
        public uint displayPixelHeight;

        /// <summary>Nominal viewer X position in display space (meters).</summary>
        public float nominalViewerX;

        /// <summary>Nominal viewer Y position in display space (meters).</summary>
        public float nominalViewerY;

        /// <summary>Nominal viewer Z position in display space (meters, distance from display).</summary>
        public float nominalViewerZ;

        /// <summary>Recommended view scale X (fraction of swapchain used).</summary>
        public float recommendedViewScaleX;

        /// <summary>Recommended view scale Y (fraction of swapchain used).</summary>
        public float recommendedViewScaleY;

        /// <summary>Whether the display supports 2D/3D mode switching.</summary>
        [MarshalAs(UnmanagedType.U1)]
        public bool supportsDisplayModeSwitch;

        /// <summary>Whether display info was successfully queried.</summary>
        [MarshalAs(UnmanagedType.U1)]
        public bool isValid;
    }
}
