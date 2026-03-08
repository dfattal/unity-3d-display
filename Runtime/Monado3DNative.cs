// Copyright 2024-2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Runtime.InteropServices;

namespace Monado.Display3D
{
    /// <summary>
    /// P/Invoke bindings to the monado3d_unity native plugin.
    /// </summary>
    internal static class Monado3DNative
    {
        private const string LibName = "monado3d_unity";

        /// <summary>
        /// Install OpenXR hook chain. Called once with the next xrGetInstanceProcAddr.
        /// Returns our hook function pointer.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr monado3d_install_hooks(IntPtr nextGetInstanceProcAddr);

        /// <summary>
        /// Set stereo rig tunables from game thread.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_set_tunables(
            float ipdFactor,
            float parallaxFactor,
            float perspectiveFactor,
            float virtualDisplayHeight,
            float invConvergenceDistance,
            float fovOverride,
            int cameraCentric);

        /// <summary>
        /// Get display info queried from runtime via XR_EXT_display_info.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_get_display_info(
            out float displayWidthM,
            out float displayHeightM,
            out uint pixelWidth,
            out uint pixelHeight,
            out float nominalX,
            out float nominalY,
            out float nominalZ,
            out float scaleX,
            out float scaleY,
            out int supportsModeSwitch,
            out int isValid);

        /// <summary>
        /// Get raw eye positions from last xrLocateViews call.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_get_eye_positions(
            out float lx, out float ly, out float lz,
            out float rx, out float ry, out float rz,
            out int isTracked);

        /// <summary>
        /// Set scene transform (parent camera pose + zoom) applied before Kooima.
        /// Chain: raw eyes → scene transform → tunables → Kooima.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_set_scene_transform(
            float posX, float posY, float posZ,
            float oriX, float oriY, float oriZ, float oriW,
            float scaleX, float scaleY, float scaleZ,
            int enabled);

        /// <summary>
        /// Set the window handle for session creation (HWND on Win32, NSView* on macOS).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_set_window_handle(IntPtr handle);

        /// <summary>
        /// Request 2D or 3D display mode.
        /// </summary>
        /// <param name="mode3d">1 for 3D mode, 0 for 2D mode.</param>
        /// <returns>1 on success, 0 on failure or not supported.</returns>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int monado3d_request_display_mode(int mode3d);

        /// <summary>
        /// Get the Kooima stereo view and projection matrices computed by the native library.
        /// These are the matched matrix pairs that should be applied directly, bypassing
        /// Unity's matrix reconstruction from (fov, position, orientation).
        /// Matrices are column-major, OpenXR/OpenGL convention.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_get_stereo_matrices(
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] leftProj,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightView,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = 16)] float[] rightProj,
            out int valid);

        /// <summary>
        /// Get readback pixel data from offscreen rendering.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_get_readback(
            out IntPtr pixels,
            out uint width,
            out uint height,
            out int ready);

        /// <summary>
        /// Create a shared GPU texture for zero-copy preview.
        /// Returns IOSurfaceRef on macOS, HANDLE on Windows, or IntPtr.Zero if not supported.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr monado3d_create_shared_texture(uint width, uint height);

        /// <summary>
        /// Destroy the shared GPU texture resources.
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_destroy_shared_texture();

        /// <summary>
        /// Get shared texture info (native pointer, dimensions, ready flag).
        /// </summary>
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void monado3d_get_shared_texture(
            out IntPtr nativePtr, out uint width, out uint height, out int ready);
    }
}
