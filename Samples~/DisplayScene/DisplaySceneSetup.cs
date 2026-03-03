// Copyright 2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D.Samples
{
    /// <summary>
    /// Creates a tabletop display-centric demo scene.
    /// A turntable with objects at varying depths demonstrates why display-centric
    /// mode is ideal for object-focused viewing: the display stays anchored in the
    /// scene and the viewer looks "into" it like a window on a table.
    /// </summary>
    public class DisplaySceneSetup : MonoBehaviour
    {
        [Tooltip("Rotation speed in degrees per second")]
        public float turntableSpeed = 15f;

        private Transform m_Turntable;

        void Start()
        {
            if (FindObjectOfType<MeshRenderer>() != null)
                return;

            // Turntable pivot — objects rotate around this
            var turntable = new GameObject("Turntable");
            turntable.transform.position = Vector3.zero;
            m_Turntable = turntable.transform;

            // Central sphere (gold) — at the display plane
            CreateSphere("CenterSphere", Vector3.zero, 0.12f,
                new Color(0.85f, 0.65f, 0.13f), m_Turntable);

            // Orbiting objects at varying depths
            // Small sphere (red) — pops out toward viewer
            CreateSphere("NearOrbit", new Vector3(0.25f, 0.08f, -0.1f), 0.06f,
                new Color(0.9f, 0.2f, 0.2f), m_Turntable);

            // Tall cylinder (teal) — extends behind display plane
            CreateCylinder("FarPillar", new Vector3(-0.2f, 0f, 0.15f), 0.04f, 0.18f,
                new Color(0.2f, 0.7f, 0.7f), m_Turntable);

            // Cube (purple) — mid-depth, offset to the side
            CreateCube("SideCube", new Vector3(0f, -0.02f, -0.22f), 0.09f,
                new Color(0.6f, 0.3f, 0.8f), m_Turntable);

            // Small floating sphere (white) — above, pops out
            CreateSphere("FloatingSphere", new Vector3(-0.12f, 0.2f, 0.05f), 0.04f,
                Color.white, m_Turntable);

            // Base platform (dark gray disc)
            var platform = GameObject.CreatePrimitive(PrimitiveType.Cylinder);
            platform.name = "Platform";
            platform.transform.position = new Vector3(0f, -0.08f, 0f);
            platform.transform.localScale = new Vector3(0.5f, 0.01f, 0.5f);
            var platMat = new Material(Shader.Find("Standard"));
            platMat.color = new Color(0.15f, 0.15f, 0.18f);
            platform.GetComponent<Renderer>().material = platMat;

            // Floor
            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            floor.transform.position = new Vector3(0f, -0.5f, 0f);
            floor.transform.localScale = new Vector3(0.5f, 1f, 0.5f);
            var floorMat = new Material(Shader.Find("Standard"));
            floorMat.color = new Color(0.25f, 0.25f, 0.25f);
            floor.GetComponent<Renderer>().material = floorMat;

            // Warm key light + cool fill
            var keyLightGo = new GameObject("KeyLight");
            var keyLight = keyLightGo.AddComponent<Light>();
            keyLight.type = LightType.Directional;
            keyLight.color = new Color(1f, 0.95f, 0.85f);
            keyLight.intensity = 1.2f;
            keyLightGo.transform.rotation = Quaternion.Euler(45f, -30f, 0f);

            var fillLightGo = new GameObject("FillLight");
            var fillLight = fillLightGo.AddComponent<Light>();
            fillLight.type = LightType.Directional;
            fillLight.color = new Color(0.7f, 0.8f, 1f);
            fillLight.intensity = 0.4f;
            fillLightGo.transform.rotation = Quaternion.Euler(30f, 150f, 0f);
        }

        void Update()
        {
            if (m_Turntable != null)
                m_Turntable.Rotate(Vector3.up, turntableSpeed * Time.deltaTime);
        }

        private void CreateSphere(string name, Vector3 pos, float radius, Color color, Transform parent)
        {
            var sphere = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            sphere.name = name;
            sphere.transform.SetParent(parent);
            sphere.transform.localPosition = pos;
            sphere.transform.localScale = Vector3.one * radius * 2f;
            var mat = new Material(Shader.Find("Standard"));
            mat.color = color;
            mat.SetFloat("_Glossiness", 0.8f);
            sphere.GetComponent<Renderer>().material = mat;
        }

        private void CreateCylinder(string name, Vector3 pos, float radius, float height, Color color, Transform parent)
        {
            var cyl = GameObject.CreatePrimitive(PrimitiveType.Cylinder);
            cyl.name = name;
            cyl.transform.SetParent(parent);
            cyl.transform.localPosition = pos + Vector3.up * height * 0.5f;
            cyl.transform.localScale = new Vector3(radius * 2f, height * 0.5f, radius * 2f);
            var mat = new Material(Shader.Find("Standard"));
            mat.color = color;
            cyl.GetComponent<Renderer>().material = mat;
        }

        private void CreateCube(string name, Vector3 pos, float size, Color color, Transform parent)
        {
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = name;
            cube.transform.SetParent(parent);
            cube.transform.localPosition = pos;
            cube.transform.localScale = Vector3.one * size;
            cube.transform.localRotation = Quaternion.Euler(15f, 45f, 10f);
            var mat = new Material(Shader.Find("Standard"));
            mat.color = color;
            cube.GetComponent<Renderer>().material = mat;
        }
    }
}
