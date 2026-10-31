using System;

namespace Il2CppNativeBridge.Tests
{
    /// <summary>
    /// 只用于生成 IL2CPP 源码的探针类型。
    ///
    /// 覆盖 primitive、reference 与 null 的 static/实例方法，供从生成代码读取
    /// runtime_invoke 的参数槽约定；不参与任何运行时验收。
    /// </summary>
    public class SourceProbe
    {
        public int retained;

        public static int Add(int left, int right)
        {
            return left + right;
        }

        public static bool Toggle(bool value)
        {
            return !value;
        }

        public static float Scale(float value)
        {
            return value * 2.0f;
        }

        public static double Half(double value)
        {
            return value * 0.5;
        }

        public static string Echo(string value)
        {
            return value;
        }

        public static object Identity(object value)
        {
            return value;
        }

        public static string Describe(object value)
        {
            return value == null ? "null" : value.ToString();
        }

        /// <summary>实例工厂：验收需要真实托管实例才能覆盖实例槽。</summary>
        public static SourceProbe Create()
        {
            return new SourceProbe();
        }

        public int Read()
        {
            return retained;
        }

        public void Write(int value)
        {
            retained = value;
        }

        public string Combine(string left, object right)
        {
            return left + (right == null ? "null" : right.ToString());
        }
    }
}
