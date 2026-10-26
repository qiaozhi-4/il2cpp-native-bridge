# 兼容性与构建证据

**记录日期**：2026-10-26

原始矩阵 JSON（`tests/results/*.json`）是执行器输出，不再入库。本文的数字由下面的命令从 JSON 复算得到，二者不一致时以脚本输出为准：

```sh
python3 tests/runtime/summarize_matrix.py --markdown \
    tests/results/android-arm64-matrix.json tests/results/host-matrix.json

# 校验本文与 JSON 是否一致
python3 tests/runtime/summarize_matrix.py \
    tests/results/android-arm64-matrix.json tests/results/host-matrix.json \
    --check tests/results/compatibility-summary.md
```

## 已通过

- 主机端 C++ 单元测试使用模拟 IL2CPP 运行时和 Frida Gum API 替身，构建及运行均通过。
- CTest 共 `13/13` 通过：`il2cpp_native_bridge_tests` 全量一次，另有 10 个分组用例（`bridge.runtime_lifecycle`、`bridge.frida_gum_backend`、`bridge.member_lookup`、`bridge.class_cache`、`bridge.field_access`、`bridge.invocation`、`bridge.hook_lifecycle`、`bridge.hook_slot`、`bridge.concurrency`、`bridge.perform_when_ready`），加上静态析构期独立用例与 Python 矩阵执行器测试。
- 三种构建配置下均为零告警且全部用例通过：常规 `-Werror`、`-fsanitize=address,undefined`、`-fsanitize=thread`。并发用例在 TSan 下无数据竞争报告。
- Android `arm64-v8a` 桥接库构建：元数据核心和 Frida Gum 适配层使用 NDK `27.0.12077973`，基于测试用 Gum API 替身编译并链接通过。
- GameBox 使用方冒烟测试：设置 `XMG_NATIVE_BRIDGE_SMOKE=ON` 后，为 Android `arm64-v8a` 配置并链接 `il2cpp_native_bridge_smoke`，使用同一个测试替身。
- 官方依赖包：仓库只提交 `third_party/frida-gum/17.23.3/android-arm64/frida-gum-devkit-17.23.3-android-arm64.tar.xz`（10,231,876 字节），SHA-256 为 `81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85`。CMake 在配置阶段校验该摘要与解压后 `libfrida-gum.a` 的 SHA-256（`1a9d92ab…`），不匹配即终止；不下载、不从源码构建 Gum。
- 官方静态库的 Android 链接：Android ARM64 独立可执行文件和 GameBox `il2cpp_native_bridge_smoke` 共享库均使用 NDK `27.0.12077973`，在未覆盖开发工具包路径的情况下，基于解压出的官方静态库编译并链接通过。
- Unity acceptance 插件的 Android ARM64 配置、编译和链接通过，使用已安装的 NDK `29.0.14206865` 与解压出的 Frida Gum 静态库；这只验证原生链接，不代表 Unity 玩家或设备运行通过。
- 现有原生库构建回归：`core`、`jxlmt`、`mrfz`、`wdyz` 和 `whmx` 均通过 `release.py build-so` 构建，并生成已校验的本地清单。
- 新增 Unity acceptance 原生插件在 macOS ARM64 主机上通过 CMake 编译；该结果只验证 C++ fixture 可构建，不代表 Unity 或 IL2CPP 运行时验证。
- 包化：`cmake --install` 产出的安装树可被独立工程以 `find_package(il2cpp_native_bridge CONFIG REQUIRED)` 消费，主机与 Android 两端均构建链接通过；版本约束 `0.1` 通过、`0.2` 明确拒绝。

## 矩阵结果

### Android ARM64（`android-arm64-v8a`）

共 9 个样例：`failed` 2、`blocked` 7。

blocked 原因分布：`no_editor` 7。

| Unity 版本 | 状态 | 原因 |
|---|---|---|
| `2017.4.16f1` | `failed` | `—` |
| `2017.4.40f1` | `blocked` | `no_editor` |
| `2018.3.0f1` | `blocked` | `no_editor` |
| `2019.3.0f1` | `blocked` | `no_editor` |
| `2021.2.0f1` | `blocked` | `no_editor` |
| `2021.3.39f1` | `blocked` | `no_editor` |
| `2023.2.20f1` | `blocked` | `no_editor` |
| `6000.1.0f1` | `blocked` | `no_editor` |
| `6000.3.10f1` | `failed` | `—` |

### Linux x64 主机（`linux-x64`）

共 9 个样例：`failed` 1、`blocked` 8。

blocked 原因分布：`host_platform_mismatch` 8。

| Unity 版本 | 状态 | 原因 |
|---|---|---|
| `2017.4.40f1` | `blocked` | `host_platform_mismatch` |
| `2018.3.0f1` | `blocked` | `host_platform_mismatch` |
| `2019.3.0f1` | `blocked` | `host_platform_mismatch` |
| `2021.2.0f1` | `blocked` | `host_platform_mismatch` |
| `2023.2.20f1` | `blocked` | `host_platform_mismatch` |
| `5.3.0f4` | `blocked` | `host_platform_mismatch` |
| `5.3.5f1` | `blocked` | `host_platform_mismatch` |
| `6000.1.0f1` | `blocked` | `host_platform_mismatch` |
| `6000.3.10f1` | `failed` | `—` |

## 原因分类与解除条件

`blocked` 只允许以下五类，`run_matrix.py` 写入时会校验：

| 分类 | 含义 | 解除条件 |
|---|---|---|
| `no_editor` | Unity Editor 可执行文件缺失，或许可证未激活 | 安装对应版本 Editor 并通过 `--unity-editor` 或 `UNITY_EDITOR` 指定；激活许可证 |
| `no_device` | Android 设备缺失或未就绪 | 连接 `arm64-v8a` 设备并设置 `ANDROID_SERIAL` 或 `--device` |
| `no_ndk` | Android NDK 缺失 | 设置 `--ndk`、`ANDROID_NDK_HOME` 或 `ANDROID_NDK_ROOT` |
| `toolchain_missing` | adb / cmake 缺失，或原生插件构建前置条件不满足 | 补齐构建工具 |
| `host_platform_mismatch` | 主机端 fixture 要求 Linux x86_64 | 在 Linux x86_64 主机上运行 |

## 尚未验证

- **没有任何 Unity 运行时结果**。两个矩阵里 `passed` 均为 0；所有 `blocked` 都停在 Editor 或平台前置条件，`UnityEngine.Time.set_timeScale` 的查找、调用、Gum Hook、移除和恢复流程从未在真实 IL2CPP 上执行过。
- Android 的 2 个 `failed`（`2017.4.16f1`、`6000.3.10f1`）是 Unity 玩家构建失败，不是环境缺失。`6000.3.10f1` 的日志停在 `System.Runtime.Numerics.dll` 相关的 Facades 路径，`2017.4.16f1` 只留下「Unity Android player build failed」而没有更细的摘要；两者都需要在对应 Editor 上重跑并保留完整日志才能定性。
- Docker 主机端尝试运行 Unity `6000.3.10f1`。官方 Editor 与 Linux IL2CPP Support 下载、解压均成功；随后 `linux/amd64` 模拟容器中的 Unity Mono 在生成 `GameAssembly.dll` 时于 `x86-codegen.h:410` 断言失败并报告 `System.OutOfMemoryException`。本机 Docker daemon 是 ARM64；需使用能正常运行 Unity x86_64 Mono 的 Docker 模拟环境，或改用 Linux x86_64 主机。
- 兼容范围 5.3.0–6000.3.x 是**目标范围**，不是已验证结论。详见 [`docs/compatibility.md`](../../../docs/compatibility.md)。
- 设备侧：`192.168.31.42:33445` 曾连接且 ABI 为 `arm64-v8a`，但 Android 玩家尚未构建、安装或运行，因此没有任何设备端证据。
