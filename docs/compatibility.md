# 兼容性与支持

## 目标范围

库目标覆盖 Unity IL2CPP 5.3.0 至 6000.3.x，调用方无需指定 Unity 版本。产品 ABI 为 Android `arm64-v8a`；其他 Android ABI 在 CMake 配置阶段报错。实现在运行时解析 IL2CPP 导出函数，用其托管元数据 API 查询类、方法和字段；Frida Gum 的 Android ARM64 静态开发工具包仅用于原生 Hook。

## 支持等级

ABI profile 按 family 与版本两级判定，等级决定托管方法替换能否安装：

| 等级 | 范围 | 行为 |
|---|---|---|
| `approved` | 已验证版本：`2021.3`、`2022.3`、`6000.3` | 允许安装替换 |
| `candidate` | 同 `modern-static` family 的其他版本 | 允许安装替换，但不声称已在该版本验证 |
| `rejected` | `legacy-static-slot`（5.3–2018.2） | 拒绝安装替换，报 `unsupported_runtime` |

family 由版本决定：2018.3 起为 `modern-static`，2018.2 及更早为 `legacy-static-slot`。`approved` 列表在 [`src/hooks/abi_profile.cpp`](../src/hooks/abi_profile.cpp) 的 `kVerifiedVersions` 中维护，按 major.minor 匹配。形态相同不等于已在某版本验证，因此同 family 的其他版本保持 `candidate`；`legacy-static-slot` 缺少可核对的生成源码，保持 `rejected`。

查询、字段访问与方法调用不受等级限制，等级只约束替换。判定版本需要读取 `UnityEngine.Application.get_unityVersion`，并需要 `il2cpp_runtime_invoke` 与字符串访问导出；缺失时按对应错误码拒绝，见下文的部署要求。

## 部署要求

- **保留版本 getter**：替换安装会读取 `UnityEngine.Application.get_unityVersion` 选择 ABI profile。IL2CPP 只为**可达**方法生成代码；宿主若裁掉该 getter，替换会以 `not_found` 被拒绝，查询与 `invoke` 不受影响。保留方式是在托管侧引用一次该属性，或在 `link.xml` 中保留。
- **入口必须可执行**：地址校验采用 fail-closed，`methodPointer` 必须落在可执行映射内，否则以 `entry_point_unavailable` 拒绝安装。在只有 ARM 翻译层、代码映射缺少执行权限的环境里替换会被明确拒绝，不会放宽校验后交给 Gum 去 patch 未标记为可执行的代码。
- **Gum 后端**：`implementation` 依赖 Frida Gum 后端。仅使用查询与调用的宿主可以关闭 Gum 构建目标；安装替换时需链接 `il2cpp_native_bridge_frida_gum`。

## 构建

Android 平台的 CMake 构建默认把 `IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND` 设为 `ON`，并链接固定版本的 Frida Gum 17.23.3 Android ARM64 开发工具包。仓库只提交官方压缩包 `third_party/frida-gum/17.23.3/android-arm64/frida-gum-devkit-17.23.3-android-arm64.tar.xz`，配置阶段校验其 SHA-256、解压到构建目录，再校验解压后 `libfrida-gum.a` 的 SHA-256，然后链接；配置与构建都不联网，也不从源码构建 Gum。提供自备的已解压 devkit 时使用 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT`，此时跳过摘要校验。

两个摘要的默认值分别为 `81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85`（压缩包）和 `1a9d92abe132cee87686df3e95b57d7610d8239b29cd3e5710108fe832727852`（`libfrida-gum.a`），可通过 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256` 与 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256` 覆盖；任一不符即在配置阶段终止。非 Android 构建默认关闭 Gum 后端，不链接 Android 静态库。

```sh
cmake -S . -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28
cmake --build build/android-arm64 --target il2cpp_native_bridge il2cpp_native_bridge_frida_gum
```

作为子目录被消费时，`IL2CPP_NATIVE_BRIDGE_INSTALL` 默认关闭，不会向宿主注册安装规则。需要包化时用 `cmake --install` 生成可 `find_package` 的安装树；交叉编译的消费方还需传 `-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH`，否则 Android 工具链会把 `CMAKE_PREFIX_PATH` 排除在外。
