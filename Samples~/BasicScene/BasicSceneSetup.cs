// Copyright 2026, Monado 3D Display contributors
// SPDX-License-Identifier: BSL-1.0

using UnityEngine;

namespace Monado.Display3D.Samples
{
    /// <summary>
    /// Creates a minimal stereo test scene with colored cubes at varying depths.
    /// Attach to any GameObject, or use the included BasicScene.unity scene file.
    /// </summary>
    public class BasicSceneSetup : MonoBehaviour
    {
        void Start()
        {
            // Only create objects if the scene is empty (no cubes present)
            if (FindAnyObjectByType<MeshRenderer>() != null)
                return;

            // Near cube (red) — pops out of screen
            CreateCube("NearCube", new Vector3(-0.3f, 0f, 0.3f), 0.15f, Color.red);

            // Mid cube (green) — at screen plane
            CreateCube("MidCube", new Vector3(0f, 0f, 0.5f), 0.2f, Color.green);

            // Far cube (blue) — behind screen
            CreateCube("FarCube", new Vector3(0.3f, 0f, 1.0f), 0.25f, new Color(0.2f, 0.4f, 1f));

            // Floor plane
            var floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
            floor.name = "Floor";
            floor.transform.position = new Vector3(0f, -0.3f, 0.5f);
            floor.transform.localScale = new Vector3(0.3f, 1f, 0.3f);
            var floorMat = new Material(Shader.Find("Standard"));
            floorMat.color = new Color(0.3f, 0.3f, 0.3f);
            floor.GetComponent<Renderer>().material = floorMat;

            // Directional light
            var lightGo = new GameObject("DirectionalLight");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
        }

        private void CreateCube(string name, Vector3 position, float size, Color color)
        {
            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = name;
            cube.transform.position = position;
            cube.transform.localScale = Vector3.one * size;

            // Slowly rotate for visual interest
            cube.transform.rotation = Quaternion.Euler(15f, 30f, 0f);

            var mat = new Material(Shader.Find("Standard"));
            mat.color = color;
            cube.GetComponent<Renderer>().material = mat;
        }
    }
}
