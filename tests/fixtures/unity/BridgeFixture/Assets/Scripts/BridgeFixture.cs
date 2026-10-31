using System;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;

namespace Il2CppNativeBridge.Tests
{
    /// <summary>
    /// 玩家侧编排：旧 Time smoke 在主线程跑，新门面在后台线程异步提交，
    /// 主线程只在 Update 里轮询完成标志，不做同步等待。
    /// </summary>
    public sealed class BridgeFixture : MonoBehaviour
    {
        public static int retainedStaticField = 37;

        /// <summary>
        /// 保留 Application.unityVersion：IL2CPP 只生成可达方法的代码，
        /// profile bootstrap 需要这个 getter 存在，否则替换会按 not_found 拒绝。
        /// </summary>
        public static string retainedUnityVersion = Application.unityVersion;

        [DllImport("il2cpp_native_bridge_acceptance", CallingConvention = CallingConvention.Cdecl)]
        private static extern void BridgeAcceptanceBegin();

        [DllImport("il2cpp_native_bridge_acceptance", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr BridgeAcceptanceRunFacade();

        [DllImport("il2cpp_native_bridge_acceptance", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr BridgeAcceptanceFinish();

        private Thread worker;
        private volatile bool workerFinished;
        private string workerError;
        private bool reported;

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
            retainedUnityVersion = Application.unityVersion;
            float originalTimeScale = Time.timeScale;
            Time.timeScale = originalTimeScale;
            retainedStaticField = Overloaded(retainedStaticField);
            retainedStaticField = Overloaded((float)retainedStaticField);

            try
            {
                BridgeAcceptanceBegin();
            }
            catch (Exception exception)
            {
                workerError = exception.ToString();
                workerFinished = true;
                return;
            }

            worker = new Thread(RunFacadeOnWorker);
            worker.IsBackground = true;
            worker.Start();
        }

        private void RunFacadeOnWorker()
        {
            try
            {
                BridgeAcceptanceRunFacade();
            }
            catch (Exception exception)
            {
                workerError = exception.ToString();
            }
            workerFinished = true;
        }

        private void Update()
        {
            if (reported || !workerFinished) return;
            reported = true;

            string result;
            try
            {
                result = Marshal.PtrToStringAnsi(BridgeAcceptanceFinish());
            }
            catch (Exception exception)
            {
                result = "{\"schemaVersion\":1,\"error\":\"" + Escape(exception.ToString()) + "\"}";
            }

            if (!String.IsNullOrEmpty(workerError))
            {
                Debug.LogError("IL2CPP_NATIVE_BRIDGE_WORKER_ERROR:" + Escape(workerError));
            }
            string payload = result ?? "{}";
            Debug.Log("IL2CPP_NATIVE_BRIDGE_RESULT:" + payload);
            // logcat 会截断长行，完整结果同时写到应用私有目录，供 run-as 读取。
            try
            {
                System.IO.File.WriteAllText(
                    System.IO.Path.Combine(Application.persistentDataPath, "bridge-result.json"), payload);
            }
            catch (Exception exception)
            {
                Debug.LogError("IL2CPP_NATIVE_BRIDGE_RESULT_WRITE_FAILED:" + Escape(exception.ToString()));
            }
            Application.Quit();
        }

        private static string Escape(string value)
        {
            return value.Replace("\\", "\\\\").Replace("\"", "\\\"")
                .Replace("\r", "\\r").Replace("\n", "\\n");
        }
    }
}
