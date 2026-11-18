# il2cpp-native-bridge

一个轻量级 C++20 IL2CPP 运行时桥接库。它按托管名称查找类、方法与字段，在 C++ 中完成调用与托管方法替换：运行时发现已加载的 `libil2cpp.so` 导出，调用方无需指定 Unity 版本、提供元数据指针或填写原生地址。原生函数 Hook 通过 Frida Gum 的 C `Interceptor` API 实现。

主要能力：

- 按程序集、类、方法名与参数签名定位托管成员，重载可逐层收窄，不做静默猜测。
- 静态与实例方法的类型化调用，值类型与引用类型字段的有类型读写。
- 托管方法替换（implementation）与恢复（revert），由库强拥有替换记录。
- 异步统一入口 `Il2Cpp::perform`，内部完成等待运行时、附加线程、选择后端和进入访问作用域。
- 运行时状态快照、能力检测和结构化错误，便于诊断与重试决策。
- Debug 构建用 `INB` 标签输出 logcat 轨迹：首次解析的导出表、类/方法/字段解析、Hook 安装与 `perform` 生命周期，同一查询只打印一次；Release 只保留错误行，轨迹全部编译期丢弃。

## 快速上手

日常接入只需包含 `il2cpp_native_bridge/il2cpp.hpp`，在一次 `Il2Cpp::perform` 中完成查询、调用与替换：

```cpp
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <cstdio>

// 替换回调是普通函数（无捕获），由库在每次调用时传入上下文。
void on_set_time_scale(INB::ReplacementCall<void, float>& call, float value) {
    call.original(value * 2.0f);  // 保留实例与隐藏 MethodInfo，转发原实现
}

int main() {
    auto submitted = INB::Il2Cpp::perform([] {
        auto time = INB::Il2Cpp::domain()
                            .assembly("UnityEngine.CoreModule")
                            .image()
                            .klass("UnityEngine.Time");

        auto setter = time.method("set_timeScale", 1);
        auto getter = time.method("get_timeScale", 0);

        setter.invoke<void, float>(0.5f);
        float observed = getter.invoke<float>();
        std::printf("timeScale=%.3f\n", static_cast<double>(observed));

        setter.implementation(&on_set_time_scale);  // 安装替换
        setter.revert();                            // 恢复原实现
    });

    return submitted ? 0 : 1;
}
```

- `perform` 把动作提交到库的协调线程异步、串行执行，返回值只表示提交是否被接受；最终结果通过 `completion` 回调送达。默认单参重载会把失败写到诊断输出。
- 对象链 `domain → assembly → image → klass → method` 把类解析限定在选定 image 内，同名类在不同程序集不会互相污染。
- 替换回调内同样处于访问作用域，可直接继续调用 `INB::Il2Cpp::domain()`。
- 语言层的边界约定、错误处理与部署前提见 [`docs/api.md`](docs/api.md)。

完整可编译示例见 [`examples/standalone`](examples/standalone)。

## 两层 API

库提供两层公开接口，按所需的控制粒度选择：

| 层 | 头文件 | 适用 |
|---|---|---|
| 统一门面 | `il2cpp_native_bridge/il2cpp.hpp` | 一次 `perform` 完成等待就绪、附加线程、查询、调用与替换；库内部处理作用域、后端与生命周期。日常接入从这里开始。 |
| 底层 API | `bridge.hpp`、`hook_backend.hpp`、`hook_slot.hpp`、`frida_gum_hook_backend.hpp` | 需要显式控制线程附加、等待策略、Hook 保活与后端选择时能直接编排每一步。 |

两层共享同一个运行时 context，宿主通过 `Runtime::set_symbol_lookup` 配置一次的符号解析器对两者同时生效。C++ 命名空间统一为 `INB`，而头文件目录、CMake 目标名与包名保持 `il2cpp_native_bridge` 不变。

`collections.hpp` 提供字符串转换与托管数组、`List<T>`、字典的底层视图；`managed_types.hpp` 在其上提供 `ManagedString`/`ManagedList`/`ManagedDictionary` 便捷包装。它们不引入第三层运行时，与两层 API 共用同一个 context。

## 平台与依赖

- 产品目标为 Android `arm64-v8a`；其他 Android ABI 会在配置阶段报错。库只面向 Android 构建：日志直接依赖 `<android/log.h>`，符号查找依赖 `<dlfcn.h>`，没有 host 构建。
- 目标兼容范围为 Unity 5.3.0–6000.3.x。ABI profile 按 family 与版本分级：`modern-static` 的已验证版本为 `approved`，同 family 其他版本为 `candidate`，`legacy-static-slot`（5.3–2018.2）为 `rejected`。支持范围与前提见 [`docs/compatibility.md`](docs/compatibility.md)。
- Android 构建链接固定版本的 Frida Gum 17.23.3 官方预编译 devkit。仓库只提交约 10 MB 的官方压缩包 [`frida-gum-devkit-17.23.3-android-arm64.tar.xz`](third_party/frida-gum/17.23.3/android-arm64)，CMake 在配置阶段校验其 SHA-256、解压到构建目录并校验解压后 `libfrida-gum.a` 的 SHA-256，再参与链接。配置与构建都不联网，也不从源码构建 Gum。改用自备 devkit 时设置 `IL2CPP_NATIVE_BRIDGE_FRIDA_GUM_ROOT`。
- 库使用 Gum 的原生 C Hook 能力，不包含 Frida Gadget、frida-core、GumJS 或 JavaScript 引擎。桥接本身不依赖 GameBox 或 BNM；使用方可以独立选择各自的 Hook 方案。

## 构建

作为 CMake 子目录引入时默认不注册安装规则。Android 交叉编译示例：

```sh
cmake -S . -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28
cmake --build build/android-arm64 --target il2cpp_native_bridge il2cpp_native_bridge_frida_gum
```

主要目标与选项：

- `il2cpp_native_bridge`（别名 `il2cpp_native_bridge::il2cpp_native_bridge`）：核心静态库。`il2cpp_native_bridge::perform` 是转发到核心库的门面接口目标。
- `il2cpp_native_bridge_frida_gum`（别名 `il2cpp_native_bridge::frida_gum`）：Frida Gum Hook 后端，Android 默认开启；仅需查询与调用时可关闭。
- `IL2CPP_NATIVE_BRIDGE_INSTALL`、`IL2CPP_NATIVE_BRIDGE_WERROR`、`IL2CPP_NATIVE_BRIDGE_SANITIZE`、`IL2CPP_NATIVE_BRIDGE_ENABLE_LOGCAT`、`IL2CPP_NATIVE_BRIDGE_BUILD_FRIDA_GUM_BACKEND` 控制构建行为。

需要包化时用 `cmake --install` 生成安装树，消费方以 `find_package(il2cpp_native_bridge CONFIG REQUIRED)` 引入；交叉编译的消费方需额外传 `-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH`。

## 文档

- [C++ 接入 API](docs/api.md)：两层 API 的契约、类型映射、Hook、错误模型与部署要求。
- [集合与字符串工具](docs/collections.md)：`collections.hpp` 与 `managed_types.hpp` 的 API、示例和不可变字符串说明。
- [兼容性与验证](docs/compatibility.md)：支持范围、部署前提与构建选项。
- [独立示例](examples/standalone)：最小消费者，含源码树与 `find_package` 两种消费方式。
- [第三方许可证说明](LICENSES/README.md)。

## 许可证

本仓库编写的桥接代码采用 MIT 许可证（见 [`LICENSE`](LICENSE)）。Frida Gum 使用附带静态链接例外的 wxWindows Library Licence 3.1，全文见 [`LICENSES/FRIDA_GUM_COPYING.txt`](LICENSES/FRIDA_GUM_COPYING.txt)。
