using System;

namespace Il2CppNativeBridge.Tests
{
    /// <summary>
    /// 与 ProbeB 同名同命名空间的探针类型，用来验证「类解析必须限定在选定的 image 内」。
    /// 两个程序集各自编译一份，行为刻意不同。
    /// </summary>
    public class SharedProbe
    {
        public int retained;

        public static string AssemblyTag()
        {
            return "ProbeA";
        }

        public static int Add(int left, int right)
        {
            return left + right;
        }

        public static string Echo(string value)
        {
            return value + "@A";
        }

        public static object Identity(object value)
        {
            return value;
        }

        public static string Describe(object value)
        {
            return value == null ? "null@A" : "value@A";
        }

        public static int Overloaded(int value)
        {
            return value + 1;
        }

        public static int Overloaded(string value)
        {
            return value.Length + 1;
        }

        public int Read()
        {
            return retained;
        }

        public void Write(int value)
        {
            retained = value;
        }
    }
}
