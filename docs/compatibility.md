# 兼容性与验证

## 目标范围

本库目标支持 Unity IL2CPP 5.3.0 至 6000.3.x，调用方无需指定 Unity 版本。产品 ABI 为 Android `arm64-v8a`。实现会在运行时解析 IL2CPP 导出函数，并使用其托管元数据 API 查询类、方法和字段。Frida Gum 的 Android ARM64 静态开发工具包仅用于原生函数 Hook。

Unity 5.3.0f4 仅作为测试主机上的运行时样例，因为该 Unity 系列不能生成 Android ARM64 玩家版本。Android ARM64 端到端验证从支持 ARM64 的最低版本 Unity 2017.4.16f1 开始。抽样版本矩阵不代表每个补丁版本都已测试。

## 构建

Android 平台的 CMake 构建默认将 `IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND` 设为 `ON`，并链接固定版本的 Frida Gum 17.23.3 Android ARM64 开发工具包。仓库只提交官方压缩包 `third_party/frida-gum/17.23.3/android-arm64/frida-gum-devkit-17.23.3-android-arm64.tar.xz`，配置阶段解压到构建目录后再链接；配置和构建都不联网，也不会从源码构建 Gum。只有在明确提供其他已解压的开发工具包时，才使用可选配置 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT`。主机端单元测试使用 API 替身，不链接 Android 静态库。

CMake 会校验压缩包的 SHA-256（默认固定为 17.23.3 的摘要，`IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256`）和解压后 `libfrida-gum.a` 的 SHA-256（`IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256`），任一不符时在配置阶段终止。

```sh
cmake -S . -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DIL2CPP_NATIVE_BRIDGE_BUILD_TESTS=OFF
cmake --build build/android-arm64 --target il2cpp_native_bridge il2cpp_native_bridge_frida_gum
```

## 尚待运行时验证

每个 Unity 样例都应记录精确的 Editor 版本、玩家目标和 ABI、代码裁剪设置，以及运行时发现、类/方法/字段查询、方法调用和 Hook 生命周期的独立结果。Android ARM64 样例还需验证 `UnityEngine.Time.set_timeScale(float)` 的查找、调用、Gum Hook、原函数跳板、Hook 移除和数值恢复流程。仅主机端的样例必须与 Android 验证结果分开标记。

矩阵执行器 `tests/runtime/run_matrix.py` 会根据 [`tests/runtime/matrix.json`](../tests/runtime/matrix.json) 选择样例、构建原生 acceptance 插件和 Unity 玩家，并输出每项检查的 JSON 状态。Unity Editor、Android NDK、Android ARM64 设备或指定 Editor 版本缺失时，执行器将相应样例标记为 `blocked`，不会把它算作通过。每个 Editor 版本都必须与矩阵条目精确匹配；单版本执行可传该 Editor 的可执行文件，多版本执行需使用带 `{version}` 的路径模板。

```sh
python3 tests/runtime/run_matrix.py --unity-version 5.3.0f4 --target linux-x64 --host-only \\
  --unity-editor "$UNITY_EDITOR" --output tests/results/host-matrix.json
python3 tests/runtime/run_matrix.py --unity-version 2021.3.39f1 --target android-arm64-v8a \\
  --device "$ANDROID_SERIAL" --unity-editor "$UNITY_EDITOR" --output tests/results/android-arm64-matrix.json
```

主机端 fixture 没有 UnityEngine 运行时，因此 `set_timeScale` Hook 项会报告为 `not_applicable`；主机端只验证通用 IL2CPP 元数据、字段和调用路径。`UnityEngine.Time.set_timeScale` 的真实查找与 Frida Gum Hook 验收只在 Android ARM64 玩家中执行。记录实际运行结果前，不得将矩阵条目标记为已测试。

当前本地构建证据和仍未验证的运行时检查见 [`tests/results/compatibility-summary.md`](../tests/results/compatibility-summary.md)。
