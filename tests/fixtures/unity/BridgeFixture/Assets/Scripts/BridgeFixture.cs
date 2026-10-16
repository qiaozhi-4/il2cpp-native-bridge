using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Il2CppNativeBridge.Tests
{
    public sealed class BridgeFixture : MonoBehaviour
    {
        public static int retainedStaticField = 37;

        [DllImport("il2cpp_native_bridge_acceptance", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr RunBridgeAcceptance();

        public static int Overloaded(int value)
        {
            return value + 1;
        }

        public static int Overloaded(float value)
        {
            return (int)value + 2;
        }

        private void Start()
        {
            float originalTimeScale = Time.timeScale;
            Time.timeScale = originalTimeScale;
            retainedStaticField = Overloaded(retainedStaticField);
            retainedStaticField = Overloaded((float)retainedStaticField);

            string result;
            try
            {
                result = Marshal.PtrToStringAnsi(RunBridgeAcceptance());
            }
            catch (Exception exception)
            {
                result = "{\"schemaVersion\":1,\"error\":\"" + Escape(exception.ToString()) + "\"}";
            }

            Debug.Log("IL2CPP_NATIVE_BRIDGE_RESULT:" + (result ?? "{}"));
            Application.Quit();
        }

        private static string Escape(string value)
        {
            return value.Replace("\\", "\\\\").Replace("\"", "\\\"")
                .Replace("\r", "\\r").Replace("\n", "\\n");
        }
    }
}
