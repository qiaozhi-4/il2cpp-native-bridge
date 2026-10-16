# il2cpp-native-bridge

一个轻量级 C++20 IL2CPP 运行时桥接库，可按托管名称查找类、方法和字段。库会在运行时发现已加载的 `libil2cpp.so` API，无需指定 Unity 版本。元数据查询和方法调用由 C++ 实现；原生函数 Hook 使用 Frida Gum 的 C `Interceptor` API。

首个支持的平台是 Android `arm64-v8a`。主机端构建用于运行单元测试和部分 Unity 生成的兼容性样例。目标兼容范围为 Unity 5.3.0–6000.3.x；只有测试矩阵中记录的版本具备直接验证结果。

Android `arm64-v8a` 构建链接固定版本的 Frida Gum 17.23.3 官方预编译静态开发工具包（`libfrida-gum.a`），文件和公开头文件位于 [`third_party/frida-gum/17.23.3/android-arm64`](third_party/frida-gum/17.23.3/android-arm64)。CMake 使用仓库内的副本，不会在配置阶段下载或构建 Gum。只有在明确改用其他工具包时，才设置 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT`。主机端单元测试使用 Gum API 替身，不链接 Android 工具包。

本库不包含 Frida Gadget、frida-core、GumJS 或 JavaScript 引擎，也不依赖 GameBox 或 BNM。GameBox 现有的 BNM 组件仍可继续使用 Dobby。

本仓库编写的桥接代码采用 MIT 许可证。Frida Gum 使用附带静态链接例外的 wxWindows Library Licence 3.1，详见 [`LICENSES/FRIDA_GUM_COPYING.txt`](LICENSES/FRIDA_GUM_COPYING.txt)。

C++ API 见 [`docs/api.md`](docs/api.md)，验证范围和证据边界见 [`docs/compatibility.md`](docs/compatibility.md)。Unity fixture 的矩阵入口为 `tests/runtime/run_matrix.py`。它会构建原生 acceptance 插件和对应 Unity 玩家，在玩家进程中检查运行时发现、按名称查询、字段访问、重载选择、托管调用，以及 Android ARM64 上的 `UnityEngine.Time.set_timeScale` Frida Gum Hook 生命周期。没有所需 Editor、NDK 或设备时，结果会记录为 `blocked`。
