// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEditor;
using UnityEngine;

namespace DisplayXR.Editor
{
    /// <summary>
    /// Ensures standalone Player Settings are configured for windowed mode.
    /// Runs once on editor load; re-runs if settings drift back to fullscreen.
    /// Prevents the Build-and-Run and native XR play-mode windows from opening
    /// as exclusive fullscreen, giving users a title bar to close/move the window.
    /// </summary>
    [InitializeOnLoad]
    static class DisplayXRPlayerSettings
    {
        static DisplayXRPlayerSettings()
        {
            if (PlayerSettings.fullScreenMode != FullScreenMode.Windowed)
            {
                PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
                Debug.Log("[DisplayXR] Set Player Settings → Fullscreen Mode = Windowed");
            }
        }
    }
}
