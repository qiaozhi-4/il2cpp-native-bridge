# 兼容性与构建证据

**记录日期**：2026-10-13

## 已通过

- 主机端 C++ 单元测试使用模拟 IL2CPP 运行时和 Frida Gum API 替身，构建及运行均通过。
- Android `arm64-v8a` 桥接库构建：元数据核心和 Frida Gum 适配层使用 NDK `27.0.12077973`，基于测试用 Gum API 替身编译并链接通过。
- GameBox 使用方冒烟测试：设置 `XMG_NATIVE_BRIDGE_SMOKE=ON` 后，为 Android `arm64-v8a` 配置并链接 `il2cpp_native_bridge_smoke`，使用同一个测试替身。
- 官方依赖包：从 Frida 17.23.3 GitHub 发布页下载 `frida-gum-devkit-17.23.3-android-arm64.tar.xz`，并通过 SHA-256 `81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85` 校验。包内的 `libfrida-gum.a` 和 `frida-gum.h` 已放入 `third_party/frida-gum/17.23.3/android-arm64`。CMake 默认使用这两个文件，不会下载 Gum。
- 官方静态库的 Android 链接：Android ARM64 独立可执行文件和 GameBox `il2cpp_native_bridge_smoke` 共享库均使用 NDK `27.0.12077973`，在未覆盖开发工具包路径的情况下，基于仓库内的官方静态库编译并链接通过。
- Unity acceptance 插件的 Android ARM64 配置、编译和链接通过，使用已安装的 NDK `29.0.14206865` 与仓库内 Frida Gum 静态库；这只验证原生链接，不代表 Unity 玩家或设备运行通过。
- 现有原生库构建回归：`core`、`jxlmt`、`mrfz`、`wdyz` 和 `whmx` 均通过 `release.py build-so` 构建，并生成已校验的本地清单。
- 新增 Unity acceptance 原生插件在 macOS ARM64 主机上通过 CMake 编译；该结果只验证 C++ fixture 可构建，不代表 Unity 或 IL2CPP 运行时验证。
- CTest 共 `2/2` 通过：原有 C++ 单元测试和新增 Python 矩阵执行器测试均通过；Python 选择、结构化结果、CLI dry-run、Editor 版本选择和结果完整性测试为 `10/10` 通过。

## 尚未验证

- Docker 主机端尝试运行 Unity `6000.3.10f1`。官方 Editor 与 Linux IL2CPP Support 下载、解压均成功；随后 `linux/amd64` 模拟容器中的 Unity Mono 在生成 `GameAssembly.dll` 时于 `x86-codegen.h:410` 断言失败并报告 `System.OutOfMemoryException`。bridge 运行时检查没有开始，结果记录为 `failed`。本机 Docker daemon 是 ARM64；需使用能正常运行 Unity x86_64 Mono 的 Docker 模拟环境，或改用 Linux x86_64 主机。详细结果见 [`host-matrix.json`](host-matrix.json)。
- 主机矩阵其余 8 个版本目前只完成预检，因本机不是 Linux x86_64 且没有本地 Unity Editor 而标为 `blocked`。完整矩阵结果尚未完成。
- Android ARM64 矩阵的 9 个样例均因缺少对应 Unity Editor 而标为 `blocked`。设备 `192.168.31.42:33445` 已连接，ABI 为 `arm64-v8a`，本机 NDK `29.0.14206865` 可用；但 Android 玩家尚未构建、安装或运行，`UnityEngine.Time.set_timeScale` 的查找、调用、Gum Hook、移除和恢复流程均未执行。完整结果见 [`android-arm64-matrix.json`](android-arm64-matrix.json)。
- 兼容范围 5.3.0–6000.3.x 仍是目标范围，不是已验证结论；具体 Unity 版本运行时结果仍待在匹配的 Editor 和目标环境中实跑。
- 私有仓库 `qiaozhi-4/il2cpp-native-bridge` 已存在且本机 Git 读权限可用，但远端目前没有提交。GameBox `.gitmodules` 的 URL 只存在于工作区改动，尚未注册 commit-pinned gitlink；库的初始提交和根仓库子模块登记仍待完成。
