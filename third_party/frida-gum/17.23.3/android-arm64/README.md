# Frida Gum 17.23.3 Android ARM64 开发工具包

此目录包含 `il2cpp-native-bridge` 使用的 Android ARM64 官方预编译开发工具包文件：

- `libfrida-gum.a`
- `frida-gum.h`

这些文件来自 [Frida 17.23.3 发布页](https://github.com/frida/frida/releases/tag/17.23.3)附带的 `frida-gum-devkit-17.23.3-android-arm64.tar.xz`。下载包已按发布页提供的摘要完成校验：

```text
SHA-256 81b0566ef5b352f1c435b0212d4841b93cc490a6d6fbd6d9b8295e09213e8b85
```

解压后，仓库内文件的摘要如下：

```text
libfrida-gum.a  1a9d92abe132cee87686df3e95b57d7610d8239b29cd3e5710108fe832727852
frida-gum.h     1661de20286e0f41967c2c673257b58d42869a99aee651ce2501f3908440f117
```

Frida Gum 许可证及其静态链接例外保存在 [`../../../../LICENSES/FRIDA_GUM_COPYING.txt`](../../../../LICENSES/FRIDA_GUM_COPYING.txt)。CMake 直接链接此静态库，不会下载或构建 Frida Gum。
