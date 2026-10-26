# Frida Gum 17.23.3 Android ARM64 开发工具包

此目录只存放 `il2cpp-native-bridge` 使用的官方预编译开发工具包**压缩包**：

- `frida-gum-devkit-17.23.3-android-arm64.tar.xz`（10,231,876 字节）

包内包含：

- `libfrida-gum.a`（解压后 80,991,798 字节）
- `frida-gum.h`（解压后 2,098,701 字节）
- `frida-gum-example.c`

压缩包来自 [Frida 17.23.3 发布页](https://github.com/frida/frida/releases/tag/17.23.3)，已按发布页提供的摘要校验：

```text
SHA-256 81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85
```

解压后关键文件的摘要：

```text
libfrida-gum.a  1a9d92abe132cee87686df3e95b57d7610d8239b29cd3e5710108fe832727852
frida-gum.h     1661de20286e0f41967c2c673257b58d42869a99aee651ce2501f3908440f117
```

## 为什么不直接提交解压后的文件

`libfrida-gum.a` 有 77 MB。即使 git 会把它压缩到约 18 MB，每次克隆仍要多拉 18 MB，且每次升级 Gum 版本都会在历史中再存一份。仓库改为只提交 10 MB 的 xz 压缩包（压缩比 8.3:1），由 CMake 在配置阶段解压到**构建目录**：

```text
<build>/third_party/frida-gum/17.23.3/android-arm64/
```

构建目录可丢弃，因此解压产物不会进入版本库。首次配置增加约 2 秒解压时间；解压后写入 `.extracted` 标记，重复配置不再重复解压。

## 摘要校验

`cmake/FridaGum.cmake` 校验两个位置：

| 变量 | 校验对象 | 默认值 |
|---|---|---|
| `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ARCHIVE_SHA256` | 压缩包本身 | `81b0566e…` |
| `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_LIBRARY_SHA256` | 解压后的 `libfrida-gum.a` | `1a9d92ab…` |

任一不符即在配置阶段终止。设置 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT` 指向自备 devkit 时两项校验都跳过。

Frida Gum 许可证及其静态链接例外保存在 [`../../../../LICENSES/FRIDA_GUM_COPYING.txt`](../../../../LICENSES/FRIDA_GUM_COPYING.txt)。CMake 直接链接解压后的静态库，不会下载或构建 Frida Gum。
