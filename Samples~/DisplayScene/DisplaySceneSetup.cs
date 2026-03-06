// Copyright 2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D.Samples
{
    /// <summary>
    /// Reproduces the native cube_shared_metal_macos test app in Unity.
    /// Creates a textured wooden crate on a grid floor with two cameras:
    /// one display-centric at the origin, one camera-centric behind the crate.
    /// Press C to toggle between them. Full stereo rig controls via
    /// keyboard/mouse matching the native app's bindings.
    /// </summary>
    public class DisplaySceneSetup : MonoBehaviour
    {
        [Tooltip("Crate rotation speed in degrees per second")]
        public float crateRotationSpeed = 30f;

        [Tooltip("Movement speed in meters per second")]
        public float moveSpeed = 0.1f;

        [Tooltip("Mouse look sensitivity")]
        public float lookSensitivity = 0.005f;

        // View state for each camera (indexed by m_CameraMode: false=display, true=camera)
        private float m_DisplayYaw, m_DisplayPitch;
        private float m_CameraYaw, m_CameraPitch;

        // Scene objects
        private Transform m_Crate;

        // Two cameras with their rigs
        private Camera m_DisplayCam;
        private Camera m_CameraCam;
        private Monado3DDisplay m_DisplayRig;
        private Monado3DCamera m_CameraRig;
        private bool m_CameraMode; // false = display-centric active

        // Initial poses for reset
        private Vector3 m_DisplayCamStartPos;
        private Vector3 m_CameraCamStartPos;
        private float m_CameraCamStartFov;
        private float m_CameraRigStartConvergence;

        private float m_LastClickTime;

        void Start()
        {
            // Don't recreate if scene already has content
            if (FindAnyObjectByType<MeshRenderer>() != null)
                return;

            CreateSceneObjects();
            CreateCameras();

            // Initialize view angles to match camera rotations
            m_CameraYaw = 180f;
            m_CameraPitch = 5f;

            // Start in display-centric mode
            m_CameraMode = false;
            SetActiveCamera(false);
        }

        void Update()
        {
            if (m_Crate != null)
                m_Crate.Rotate(Vector3.up, crateRotationSpeed * Time.deltaTime);

            HandleInput();
            HandleMovement();
        }

        private void CreateSceneObjects()
        {
            // --- Crate ---
            var crate = GameObject.CreatePrimitive(PrimitiveType.Cube);
            crate.name = "Crate";
            crate.transform.position = new Vector3(0f, 0.03f, 0f);
            crate.transform.localScale = new Vector3(0.06f, 0.06f, 0.06f);
            m_Crate = crate.transform;

            var crateMat = new Material(Shader.Find("Standard"));
            var tex = LoadCrateTexture();
            if (tex != null)
                crateMat.mainTexture = tex;
            else
                crateMat.color = new Color(0.6f, 0.4f, 0.2f);
            crate.GetComponent<Renderer>().material = crateMat;

            // --- Ground grid ---
            var ground = GameObject.CreatePrimitive(PrimitiveType.Quad);
            ground.name = "GroundGrid";
            ground.transform.position = Vector3.zero;
            ground.transform.rotation = Quaternion.Euler(90f, 0f, 0f);
            ground.transform.localScale = Vector3.one;

            var gridMat = new Material(Shader.Find("Standard"));
            gridMat.color = new Color(0.4f, 0.4f, 0.4f);
            gridMat.mainTextureScale = new Vector2(20f, 20f);

            var checker = new Texture2D(2, 2, TextureFormat.RGBA32, false);
            checker.filterMode = FilterMode.Point;
            checker.wrapMode = TextureWrapMode.Repeat;
            var light = new Color(0.5f, 0.5f, 0.5f);
            var dark = new Color(0.3f, 0.3f, 0.3f);
            checker.SetPixels(new[] { light, dark, dark, light });
            checker.Apply();
            gridMat.mainTexture = checker;
            ground.GetComponent<Renderer>().material = gridMat;

            // --- Key light ---
            var keyLightGo = new GameObject("KeyLight");
            var keyLight = keyLightGo.AddComponent<Light>();
            keyLight.type = LightType.Directional;
            keyLight.color = new Color(1f, 0.95f, 0.85f);
            keyLight.intensity = 1.2f;
            keyLightGo.transform.rotation = Quaternion.Euler(45f, -30f, 0f);
        }

        private void CreateCameras()
        {
            // Destroy the default Main Camera if present — we create our own
            var existingMain = Camera.main;
            if (existingMain != null)
                Destroy(existingMain.gameObject);

            // --- Display-centric camera at origin (the display surface) ---
            var displayGo = new GameObject("DisplayCam");
            m_DisplayCam = displayGo.AddComponent<Camera>();
            m_DisplayCam.tag = "MainCamera";
            m_DisplayCam.nearClipPlane = 0.01f;
            m_DisplayCam.farClipPlane = 100f;
            displayGo.transform.position = Vector3.zero;
            displayGo.transform.rotation = Quaternion.identity;
            m_DisplayRig = displayGo.AddComponent<Monado3DDisplay>();
            m_DisplayCamStartPos = displayGo.transform.position;

            // --- Camera-centric camera behind and slightly above the crate ---
            // Crate is at (0, 0.03, 0). Place camera at Z=0.3, looking toward origin.
            var cameraGo = new GameObject("CameraCam");
            m_CameraCam = cameraGo.AddComponent<Camera>();
            m_CameraCam.nearClipPlane = 0.01f;
            m_CameraCam.farClipPlane = 100f;
            m_CameraCam.fieldOfView = 40f;
            cameraGo.transform.position = new Vector3(0f, 0.05f, 0.3f);
            cameraGo.transform.rotation = Quaternion.Euler(5f, 180f, 0f); // look toward origin
            m_CameraRig = cameraGo.AddComponent<Monado3DCamera>();
            m_CameraRig.convergenceDistance = 0.3f;
            m_CameraCamStartPos = cameraGo.transform.position;
            m_CameraCamStartFov = m_CameraCam.fieldOfView;
            m_CameraRigStartConvergence = m_CameraRig.convergenceDistance;
        }

        private void SetActiveCamera(bool cameraMode)
        {
            m_DisplayCam.enabled = !cameraMode;
            m_DisplayRig.enabled = !cameraMode;
            m_DisplayCam.tag = cameraMode ? "Untagged" : "MainCamera";

            m_CameraCam.enabled = cameraMode;
            m_CameraRig.enabled = cameraMode;
            m_CameraCam.tag = cameraMode ? "MainCamera" : "Untagged";
        }

        private Transform ActiveCamTransform =>
            m_CameraMode ? m_CameraCam.transform : m_DisplayCam.transform;

        private ref float ActiveYaw =>
            ref (m_CameraMode ? ref m_CameraYaw : ref m_DisplayYaw);

        private ref float ActivePitch =>
            ref (m_CameraMode ? ref m_CameraPitch : ref m_DisplayPitch);

        private void HandleInput()
        {
            // --- Mouse drag -> view rotation ---
            if (Input.GetMouseButton(0))
            {
                ActiveYaw -= Input.GetAxis("Mouse X") * lookSensitivity * 100f;
                ActivePitch -= Input.GetAxis("Mouse Y") * lookSensitivity * 100f;
                ActivePitch = Mathf.Clamp(ActivePitch, -80f, 80f);
                ActiveCamTransform.rotation = Quaternion.Euler(ActivePitch, ActiveYaw, 0f);
            }

            // --- Double-click -> reset view ---
            if (Input.GetMouseButtonDown(0))
            {
                float time = Time.unscaledTime;
                if (time - m_LastClickTime < 0.3f)
                    ResetView();
                m_LastClickTime = time;
            }

            // --- Scroll wheel -> parameter adjustment ---
            float scroll = Input.GetAxis("Mouse ScrollWheel");
            if (Mathf.Abs(scroll) > 0.001f)
            {
                float factor = scroll > 0 ? 1.1f : (1f / 1.1f);
                bool shift = Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift);
                bool ctrl = Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl);
                bool alt = Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt);

                if (shift)
                    AdjustIPD(factor);
                else if (ctrl)
                    AdjustParallax(factor);
                else if (alt)
                    AdjustAltParam(factor);
                else
                    AdjustMainParam(factor);
            }

            if (Input.GetKeyDown(KeyCode.Space))
                ResetView();

            if (Input.GetKeyDown(KeyCode.C))
                ToggleMode();
        }

        private void HandleMovement()
        {
            var t = ActiveCamTransform;
            float d = moveSpeed * Time.deltaTime;
            Vector3 move = Vector3.zero;

            if (Input.GetKey(KeyCode.W)) move += t.forward * d;
            if (Input.GetKey(KeyCode.S)) move -= t.forward * d;
            if (Input.GetKey(KeyCode.D)) move += t.right * d;
            if (Input.GetKey(KeyCode.A)) move -= t.right * d;
            if (Input.GetKey(KeyCode.E)) move += t.up * d;
            if (Input.GetKey(KeyCode.Q)) move -= t.up * d;

            if (move.sqrMagnitude > 0f)
                t.position += move;
        }

        private void AdjustIPD(float factor)
        {
            if (m_CameraMode)
                m_CameraRig.ipdFactor = Mathf.Clamp(m_CameraRig.ipdFactor * factor, 0f, 1f);
            else
                m_DisplayRig.ipdFactor = Mathf.Clamp(m_DisplayRig.ipdFactor * factor, 0f, 1f);
        }

        private void AdjustParallax(float factor)
        {
            if (m_CameraMode)
                m_CameraRig.parallaxFactor = Mathf.Clamp(m_CameraRig.parallaxFactor * factor, 0f, 1f);
            else
                m_DisplayRig.parallaxFactor = Mathf.Clamp(m_DisplayRig.parallaxFactor * factor, 0f, 1f);
        }

        // Alt+scroll: perspective (display) or convergence (camera)
        private void AdjustAltParam(float factor)
        {
            if (m_CameraMode)
                m_CameraRig.convergenceDistance = Mathf.Clamp(m_CameraRig.convergenceDistance * factor, 0.1f, 5f);
            else
                m_DisplayRig.perspectiveFactor = Mathf.Clamp(m_DisplayRig.perspectiveFactor * factor, 0.1f, 10f);
        }

        // Plain scroll: scale (display) or zoom/FOV (camera)
        private void AdjustMainParam(float factor)
        {
            if (m_CameraMode)
            {
                float fov = m_CameraRig.fieldOfView;
                if (fov < 1f)
                {
                    fov = m_CameraCam.fieldOfView;
                    if (fov < 1f) fov = 60f;
                }
                m_CameraRig.fieldOfView = Mathf.Clamp(fov * factor, 10f, 120f);
            }
            else
            {
                m_DisplayRig.scaleFactor = Mathf.Clamp(m_DisplayRig.scaleFactor * factor, 0.1f, 10f);
            }
        }

        private void ResetView()
        {
            if (m_CameraMode)
            {
                m_CameraYaw = 180f;
                m_CameraPitch = 5f; // slight downward look
                m_CameraCam.transform.position = m_CameraCamStartPos;
                m_CameraCam.transform.rotation = Quaternion.Euler(5f, 180f, 0f);
                m_CameraCam.fieldOfView = m_CameraCamStartFov;
                m_CameraRig.ipdFactor = 1f;
                m_CameraRig.parallaxFactor = 1f;
                m_CameraRig.convergenceDistance = m_CameraRigStartConvergence;
                m_CameraRig.fieldOfView = 0f;
            }
            else
            {
                m_DisplayYaw = 0f;
                m_DisplayPitch = 0f;
                m_DisplayCam.transform.position = m_DisplayCamStartPos;
                m_DisplayCam.transform.rotation = Quaternion.identity;
                m_DisplayRig.ipdFactor = 1f;
                m_DisplayRig.parallaxFactor = 1f;
                m_DisplayRig.perspectiveFactor = 1f;
                m_DisplayRig.scaleFactor = 1f;
            }
        }

        private void ToggleMode()
        {
            m_CameraMode = !m_CameraMode;
            SetActiveCamera(m_CameraMode);
            Debug.Log($"[Monado3D] Active: {(m_CameraMode ? "CameraCam (camera-centric)" : "DisplayCam (display-centric)")}");
        }

        private Texture2D LoadCrateTexture()
        {
            var tex = Resources.Load<Texture2D>("Wood_Crate_001_basecolor");
            if (tex != null) return tex;

            var allTex = Resources.FindObjectsOfTypeAll<Texture2D>();
            foreach (var t in allTex)
            {
                if (t.name == "Wood_Crate_001_basecolor")
                    return t;
            }

            Debug.LogWarning("[Monado3D] Crate texture not found. Using solid color fallback.");
            return null;
        }
    }
}
