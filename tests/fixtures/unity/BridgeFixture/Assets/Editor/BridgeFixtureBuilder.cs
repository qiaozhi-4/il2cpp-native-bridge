using System;
using System.IO;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

internal static class BridgeFixtureBuilder
{
    private const string ScenePath = "Assets/BridgeFixture.unity";
    private const string PluginName = "il2cpp_native_bridge_acceptance";

    public static void Build()
    {
        string expectedVersion = RequiredEnvironment("IL2CPP_NATIVE_BRIDGE_EXPECTED_VERSION");
        if (!String.Equals(Application.unityVersion, expectedVersion, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "Selected Unity Editor version " + Application.unityVersion
                + " does not match matrix entry " + expectedVersion + ".");
        }

        string targetName = RequiredEnvironment("IL2CPP_NATIVE_BRIDGE_BUILD_TARGET");
        string outputPath = RequiredEnvironment("IL2CPP_NATIVE_BRIDGE_OUTPUT");
        BuildTarget target = (BuildTarget)Enum.Parse(typeof(BuildTarget), targetName, false);

        ConfigureScriptingBackend(target);
        ConfigureAndroidArchitecture(target);
        EnsureScene();
        ConfigureNativePlugin(target);
        SetApplicationIdentifier(target);

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
        object buildResult = BuildPipeline.BuildPlayer(
            new[] { ScenePath }, outputPath, target, BuildOptions.Development);
        string error = GetBuildFailure(buildResult);
        if (!String.IsNullOrEmpty(error))
        {
            throw new InvalidOperationException("Unity player build failed: " + error);
        }
    }

    private static void SetApplicationIdentifier(BuildTarget target)
    {
        BuildTargetGroup group = target == BuildTarget.Android
            ? BuildTargetGroup.Android
            : BuildTargetGroup.Standalone;
        MethodInfo setter = typeof(PlayerSettings).GetMethod(
            "SetApplicationIdentifier",
            BindingFlags.Public | BindingFlags.Static,
            null,
            new[] { typeof(BuildTargetGroup), typeof(string) },
            null);
        if (setter != null)
        {
            setter.Invoke(null, new object[] { group, "com.il2cppnativebridge.fixture" });
            return;
        }

        PropertyInfo legacyIdentifier = typeof(PlayerSettings).GetProperty(
            "bundleIdentifier", BindingFlags.Public | BindingFlags.Static);
        if (legacyIdentifier == null || !legacyIdentifier.CanWrite)
        {
            throw new MissingMemberException("This Unity Editor cannot set the application identifier.");
        }
        legacyIdentifier.SetValue(null, "com.il2cppnativebridge.fixture", null);
    }

    private static string GetBuildFailure(object buildResult)
    {
        if (buildResult == null) return null;

        string error = buildResult as string;
        if (error != null) return String.IsNullOrEmpty(error) ? null : error;

        PropertyInfo summaryProperty = buildResult.GetType().GetProperty("summary");
        object summary = summaryProperty == null ? null : summaryProperty.GetValue(buildResult, null);
        PropertyInfo resultProperty = summary == null ? null : summary.GetType().GetProperty("result");
        object result = resultProperty == null ? null : resultProperty.GetValue(summary, null);
        if (result != null && String.Equals(result.ToString(), "Succeeded", StringComparison.Ordinal))
        {
            return null;
        }

        PropertyInfo errorsProperty = summary == null ? null : summary.GetType().GetProperty("totalErrors");
        object errors = errorsProperty == null ? null : errorsProperty.GetValue(summary, null);
        return "result=" + (result == null ? "unknown" : result.ToString())
            + "; errors=" + (errors == null ? "unknown" : errors.ToString());
    }

    private static string RequiredEnvironment(string name)
    {
        string value = Environment.GetEnvironmentVariable(name);
        if (String.IsNullOrEmpty(value))
        {
            throw new InvalidOperationException("Missing environment variable: " + name);
        }
        return value;
    }

    private static void ConfigureScriptingBackend(BuildTarget target)
    {
        BuildTargetGroup group = target == BuildTarget.Android
            ? BuildTargetGroup.Android
            : BuildTargetGroup.Standalone;
        MethodInfo setter = null;
        foreach (MethodInfo candidate in typeof(PlayerSettings).GetMethods(
                     BindingFlags.Public | BindingFlags.Static))
        {
            ParameterInfo[] candidateParameters = candidate.GetParameters();
            if (candidate.Name == "SetScriptingBackend" && candidateParameters.Length == 2
                && candidateParameters[0].ParameterType == typeof(BuildTargetGroup)
                && candidateParameters[1].ParameterType.IsEnum)
            {
                setter = candidate;
                break;
            }
        }
        if (setter == null)
        {
            throw new MissingMethodException("This Unity Editor cannot select the IL2CPP scripting backend.");
        }

        ParameterInfo[] parameters = setter.GetParameters();
        object il2cpp = Enum.Parse(parameters[1].ParameterType, "IL2CPP", false);
        setter.Invoke(null, new[] { (object)group, il2cpp });

        foreach (MethodInfo candidate in typeof(PlayerSettings).GetMethods(
                     BindingFlags.Public | BindingFlags.Static))
        {
            ParameterInfo[] strippingParameters = candidate.GetParameters();
            if (candidate.Name != "SetManagedStrippingLevel" || strippingParameters.Length != 2
                || strippingParameters[0].ParameterType != typeof(BuildTargetGroup)
                || !strippingParameters[1].ParameterType.IsEnum)
            {
                continue;
            }
            try
            {
                object disabled = Enum.Parse(strippingParameters[1].ParameterType, "Disabled", false);
                candidate.Invoke(null, new[] { (object)group, disabled });
            }
            catch (ArgumentException)
            {
                // 较旧的 Unity Editor 没有可配置的托管裁剪级别；fixture 仍由 link.xml 保留。
            }
            break;
        }
    }

    private static void ConfigureAndroidArchitecture(BuildTarget target)
    {
        if (target != BuildTarget.Android) return;

        Type androidSettings = typeof(PlayerSettings).GetNestedType(
            "Android", BindingFlags.Public | BindingFlags.NonPublic);
        if (androidSettings == null)
        {
            throw new MissingMemberException("This Unity Editor has no Android player settings type.");
        }

        const BindingFlags flags = BindingFlags.Public | BindingFlags.Static;
        FieldInfo architecturesField = androidSettings.GetField("targetArchitectures", flags);
        PropertyInfo architecturesProperty = androidSettings.GetProperty("targetArchitectures", flags);
        Type architectureType = architecturesField != null
            ? architecturesField.FieldType
            : architecturesProperty == null ? null : architecturesProperty.PropertyType;
        if (architectureType == null || !architectureType.IsEnum
            || (architecturesField == null && !architecturesProperty.CanWrite))
        {
            throw new MissingFieldException("This Unity Editor cannot select Android ARM64 architecture.");
        }
        object arm64 = Enum.Parse(architectureType, "ARM64", false);
        if (architecturesField != null) architecturesField.SetValue(null, arm64);
        else architecturesProperty.SetValue(null, arm64, null);
    }

    private static void EnsureScene()
    {
        Directory.CreateDirectory("Assets");
        Scene scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        GameObject fixture = new GameObject("IL2CPP Native Bridge Fixture");
        fixture.AddComponent<Il2CppNativeBridge.Tests.BridgeFixture>();
        EditorSceneManager.SaveScene(scene, ScenePath);
        EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
        AssetDatabase.SaveAssets();
    }

    private static void ConfigureNativePlugin(BuildTarget target)
    {
        AssetDatabase.Refresh();
        string assetPath = target == BuildTarget.Android
            ? "Assets/Plugins/Android/arm64-v8a/" + PluginName + ".so"
            : "Assets/Plugins/x86_64/" + PluginName + ".so";
        PluginImporter importer = AssetImporter.GetAtPath(assetPath) as PluginImporter;
        if (importer == null)
        {
            throw new InvalidOperationException("Unity did not import the native bridge acceptance plugin: " + assetPath);
        }

        importer.SetCompatibleWithAnyPlatform(false);
        importer.SetCompatibleWithPlatform(target, true);
        if (target == BuildTarget.Android)
        {
            MethodInfo platformData = null;
            foreach (MethodInfo candidate in typeof(PluginImporter).GetMethods(
                         BindingFlags.Public | BindingFlags.Instance))
            {
                ParameterInfo[] parameters = candidate.GetParameters();
                if (candidate.Name == "SetPlatformData" && parameters.Length == 3
                    && parameters[0].ParameterType == typeof(BuildTarget))
                {
                    platformData = candidate;
                    break;
                }
            }
            if (platformData == null)
            {
                throw new MissingMethodException("This Unity Editor cannot select the Android ARM64 plugin CPU.");
            }
            platformData.Invoke(importer, new object[] { target, "CPU", "ARM64" });
        }
        importer.SaveAndReimport();
    }
}
