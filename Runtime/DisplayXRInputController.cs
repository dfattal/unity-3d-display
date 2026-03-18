// Copyright 2024-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;
#if HAS_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace DisplayXR
{
    /// <summary>
    /// Basic keyboard + mouse input for navigating around a DisplayXR rig.
    /// WASD = move horizontally, QE = move up/down, left-mouse drag = rotate.
    /// Scroll wheel = zoom (scale). Space = reset to initial pose.
    /// Attach to the same GameObject as DisplayXRDisplay or DisplayXRCamera.
    /// Works in Play Mode (including with the standalone preview via PlayModeIntegration).
    /// </summary>
    [AddComponentMenu("DisplayXR/Input Controller")]
    public class DisplayXRInputController : MonoBehaviour
    {
        [Header("Movement")]
        [Tooltip("Movement speed in meters per second.")]
        public float moveSpeed = 1.0f;

        [Tooltip("Mouse rotation sensitivity (radians per pixel).")]
        public float rotationSensitivity = 0.005f;

        [Tooltip("Scroll wheel zoom speed (scale factor per scroll tick).")]
        public float zoomSpeed = 0.1f;

        private float m_Yaw;
        private float m_Pitch;
        private bool m_Dragging;
        private Vector2 m_LastMousePos;

        private Vector3 m_InitialPosition;
        private float m_InitialYaw, m_InitialPitch;
        private Vector3 m_InitialScale;

        void Start()
        {

            // Ensure input works even when Game View doesn't have focus
            // (e.g. when DisplayXR Preview window is focused instead)
            Application.runInBackground = true;
#if HAS_INPUT_SYSTEM
            InputSystem.settings.backgroundBehavior = InputSettings.BackgroundBehavior.IgnoreFocus;
#endif

            Vector3 euler = transform.eulerAngles;
            m_Yaw = euler.y * Mathf.Deg2Rad;
            m_Pitch = euler.x * Mathf.Deg2Rad;
            if (m_Pitch > Mathf.PI) m_Pitch -= 2f * Mathf.PI;

            m_InitialPosition = transform.position;
            m_InitialYaw = m_Yaw;
            m_InitialPitch = m_Pitch;
            m_InitialScale = transform.localScale;
        }

        // Rendering mode cycling
        private int m_CurrentRenderingMode = 1;

        void Update()
        {
            HandleMouseRotation();
            HandleKeyboardMovement();
            HandleScrollZoom();
            HandleReset();
            HandleQuit();
            HandleModeCycle();
        }

        private void HandleMouseRotation()
        {
            if (GetMouseButtonDown(0))
            {
                m_Dragging = true;
                m_LastMousePos = GetMousePosition();
            }
            if (GetMouseButtonUp(0))
                m_Dragging = false;

            if (m_Dragging)
            {
                Vector2 pos = GetMousePosition();
                Vector2 delta = pos - m_LastMousePos;
                m_Yaw -= delta.x * rotationSensitivity;
                m_Pitch -= delta.y * rotationSensitivity;
                m_Pitch = Mathf.Clamp(m_Pitch, -1.4f, 1.4f);
                m_LastMousePos = pos;

                transform.rotation = Quaternion.Euler(
                    m_Pitch * Mathf.Rad2Deg,
                    m_Yaw * Mathf.Rad2Deg,
                    0f);
            }
        }

        private void HandleKeyboardMovement()
        {
            // Compute direction vectors from stored yaw/pitch, NOT from transform.
            // In XR mode, Unity's XR subsystem overwrites the camera transform each
            // frame with the tracked eye pose — reading transform.forward/right/up
            // gives the XR-modified orientation, not the controller's intended one.
            // This matches the reference test app (input_handler.cpp:UpdateCameraMovement).
            Quaternion ori = Quaternion.Euler(m_Pitch * Mathf.Rad2Deg, m_Yaw * Mathf.Rad2Deg, 0f);
            Vector3 fwd = ori * Vector3.forward;
            Vector3 rt = ori * Vector3.right;
            Vector3 up = ori * Vector3.up;

            Vector3 move = Vector3.zero;
            if (GetKey(KeyCode.W)) move -= fwd;
            if (GetKey(KeyCode.S)) move += fwd;
            if (GetKey(KeyCode.D)) move += rt;
            if (GetKey(KeyCode.A)) move -= rt;
            if (GetKey(KeyCode.E)) move += up;
            if (GetKey(KeyCode.Q)) move -= up;

            if (move.sqrMagnitude > 0f)
                transform.position += move.normalized * moveSpeed * Time.deltaTime;
        }

        private void HandleScrollZoom()
        {
            float scroll = GetScrollDelta();
            if (Mathf.Abs(scroll) < 0.001f) return;

            float factor = 1f + scroll * zoomSpeed;
            factor = Mathf.Clamp(factor, 0.5f, 2f);
            transform.localScale *= factor;
        }

        private void HandleReset()
        {
            if (GetKeyDown(KeyCode.Space))
            {
                transform.position = m_InitialPosition;
                transform.localScale = m_InitialScale;
                m_Yaw = m_InitialYaw;
                m_Pitch = m_InitialPitch;
                transform.rotation = Quaternion.Euler(
                    m_Pitch * Mathf.Rad2Deg,
                    m_Yaw * Mathf.Rad2Deg,
                    0f);
            }
        }

        private void HandleQuit()
        {
            if (GetKeyDown(KeyCode.Escape))
                Application.Quit();
        }

        private void HandleModeCycle()
        {
            if (!GetKeyDown(KeyCode.V)) return;

            // Toggle 2D/3D via the non-standalone API (works in built apps)
            m_CurrentRenderingMode = m_CurrentRenderingMode == 0 ? 1 : 0;
            DisplayXRNative.displayxr_request_display_mode(m_CurrentRenderingMode);
            Debug.Log($"[DisplayXR] Display mode → {(m_CurrentRenderingMode == 0 ? "2D" : "3D")}");
        }

        // --- Input abstraction (Input System vs Legacy) ---

#if HAS_INPUT_SYSTEM
        private static bool GetKey(KeyCode k) =>
            Keyboard.current != null && Keyboard.current[ToKey(k)].isPressed;
        private static bool GetKeyDown(KeyCode k) =>
            Keyboard.current != null && Keyboard.current[ToKey(k)].wasPressedThisFrame;
        private static bool GetMouseButtonDown(int b) =>
            Mouse.current != null && (b == 0 ? Mouse.current.leftButton.wasPressedThisFrame
            : b == 1 ? Mouse.current.rightButton.wasPressedThisFrame
            : Mouse.current.middleButton.wasPressedThisFrame);
        private static bool GetMouseButtonUp(int b) =>
            Mouse.current != null && (b == 0 ? Mouse.current.leftButton.wasReleasedThisFrame
            : b == 1 ? Mouse.current.rightButton.wasReleasedThisFrame
            : Mouse.current.middleButton.wasReleasedThisFrame);
        private static Vector2 GetMousePosition() =>
            Mouse.current != null ? Mouse.current.position.ReadValue() : Vector2.zero;
        private static float GetScrollDelta() =>
            Mouse.current != null ? Mouse.current.scroll.ReadValue().y / 120f : 0f;

        private static Key ToKey(KeyCode k)
        {
            switch (k)
            {
                case KeyCode.W: return Key.W;
                case KeyCode.A: return Key.A;
                case KeyCode.S: return Key.S;
                case KeyCode.D: return Key.D;
                case KeyCode.Q: return Key.Q;
                case KeyCode.E: return Key.E;
                case KeyCode.Space: return Key.Space;
                case KeyCode.Escape: return Key.Escape;
                case KeyCode.V: return Key.V;
                default: return Key.None;
            }
        }
#else
        private static bool GetKey(KeyCode k) => Input.GetKey(k);
        private static bool GetKeyDown(KeyCode k) => Input.GetKeyDown(k);
        private static bool GetMouseButtonDown(int b) => Input.GetMouseButtonDown(b);
        private static bool GetMouseButtonUp(int b) => Input.GetMouseButtonUp(b);
        private static Vector2 GetMousePosition() => Input.mousePosition;
        private static float GetScrollDelta() => Input.mouseScrollDelta.y;
#endif
    }
}
