using System;

namespace Il2CppNativeBridge.Tests
{
    /// <summary>
    /// 与 ProbeA 同名同命名空间、行为不同的第二个程序集。
    ///
    /// 门面必须按 image 限定类解析：同名类在不同程序集里是两个不同的 Il2CppClass。
    /// </summary>
    public class SharedProbe
    {
        public int retained;

        public static string AssemblyTag()
        {
            return "ProbeB";
        }

        public static int Add(int left, int right)
        {
            return left + right + 100;
        }

        public static string Echo(string value)
        {
            return value + "@B";
        }

        public static object Identity(object value)
        {
            return value;
        }

        public static string Describe(object value)
        {
            return value == null ? "null@B" : "value@B";
        }

        public static int Overloaded(int value)
        {
            return value + 2;
        }

        public static int Overloaded(string value)
        {
            return value.Length + 2;
        }

        public int Read()
        {
            return retained + 100;
        }

        public void Write(int value)
        {
            retained = value + 100;
        }
    }
}
