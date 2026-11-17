# C++ 接入 API

桥接层通过运行时导出函数解析 IL2CPP 元数据。调用方无需提供 Unity 版本、原始元数据指针或原生地址。

```cpp
#include <optional>
#include "il2cpp_native_bridge/bridge.hpp"
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"
#include "il2cpp_native_bridge/hook_slot.hpp"

using namespace INB;

auto runtime = Runtime::current();
std::optional<HookHandle<void, float>> time_scale_hook;
auto hook_result = runtime.with_attached_thread([&]() -> Result<void> {
    return with_frida_gum_hook_backend([&](const auto& backend) -> Result<void> {
        auto time = runtime.klass("UnityEngine.Time");
        auto setter = time.method<void, float>("set_timeScale");
        auto installed = setter.hook(backend, replacement);
        if (!installed) return Result<void>::failure(installed.error());
        time_scale_hook.emplace(installed.take_value());
        return Result<void>::success();
    });
});
if (!hook_result) return;
```

`time_scale_hook` 应保存在组件状态中，并保持有效直到显式移除或组件清理。

`klass` 会搜索已加载的程序集，并要求结果唯一。需要缩小搜索范围时可使用 `in_assembly(...)`。`method<Return, Args...>` 按精确的托管签名选择方法；找不到方法或匹配结果不唯一时会返回错误，不会静默选择某个重载。`field<T>(name)` 通过 IL2CPP 字段 API 提供有类型的静态或实例字段读写。

`Runtime::with_attached_thread(action)` 在调用 `action` 前检查 IL2CPP runtime 是否就绪，并在整个回调期间确保当前线程已附加；若线程由 bridge 附加，会在回调结束时自动分离。runtime 未就绪或线程附加失败时，action 不会执行并返回对应错误。该接口不会等待 runtime、切换到 Unity 主线程或安排重试，调用方仍负责认证、渲染等宿主条件和重试策略。需要控制更长线程附加生命周期时，仍可直接使用 `ThreadAttachment::attach(runtime)`。

若 Hook 初始化需要等待 Unity 加载，可调用 `Runtime::perform_when_ready(action, completion, options)`。bridge 会在后台线程轮询模块、`il2cpp_get_corlib` 和程序集状态；等待超过 `options.timeout` 时最多调用一次 `options.on_timeout`，随后继续等待，直到 runtime 就绪或发现不支持的运行时。就绪后 bridge 自动附加线程并且只执行一次 action。action、线程附加或 backend 安装返回错误时，结果交给 completion，不会重新执行 action。启动成功后 completion 恰好调用一次；启动失败时直接返回错误，不调用 completion。

使用 Frida Gum 安装 Hook 时，可改用 `perform_when_ready_with_frida_gum_hook_backend(runtime, action, completion, options)`。该接口组合上述等待与线程附加流程，并在就绪后创建和检查 Gum backend；backend 不可用或 action 失败时只将错误交给 completion，不会重试安装。

GameBox 等使用自定义 ELF 扫描器的宿主，可以通过 `Runtime::set_symbol_lookup(module, lookup, is_ready)` 注册解析器和就绪检查。该方法只设置解析器，不做初始化等待；必须在调用任何 runtime 操作之前调用，参数为空或 runtime 已进入 ready 时记录诊断并终止进程。以**完全相同**的三个参数重复调用没有副作用。

需要构造托管闭合泛型类时，`Runtime::generic_class(...)` 接受开放泛型类名、类型实参和可选程序集名。它通过 IL2CPP reflection 将开放类与类型实参转换成闭合类句柄，随后可沿用普通类查询 API：

```cpp
constexpr std::array<std::string_view, 1> arguments{"System.UInt32"};
auto list_class = runtime.generic_class("System.Collections.Generic.List`1", arguments, "mscorlib");
auto list = list_class.new_object();
if (list) (void) list_class.method<void>(".ctor", 0).invoke_on(list.value());
```

此操作需要 `il2cpp_class_get_type`、`il2cpp_type_get_object`、`il2cpp_array_new` 和 `il2cpp_class_from_system_type` 等 reflection 导出；缺少所需导出时会返回 `missing_api`。

## 只读元数据查看

调试代码可显式枚举已加载 images 中的类，再查看类名、程序集名、方法签名和字段元数据：

```cpp
auto classes = runtime.classes("Torappu.Battle");
if (classes) {
    for (const auto& klass : classes.value()) {
        LOGD("%.*s [%.*s]", static_cast<int>(klass.full_name().size()), klass.full_name().data(),
             static_cast<int>(klass.assembly_name().size()), klass.assembly_name().data());
        auto methods = klass.methods();
        auto fields = klass.fields();
        /// 按需记录 methods.value() 与 fields.value() 中的元数据。
    }
}
```

`Runtime::classes(filter)` 按托管完整类名子串过滤当前已加载的类；空字符串会枚举全部类。`ClassQuery::methods()` 返回每个方法的名称、返回类型、参数类型和静态标记；字段信息沿用 `fields()`。枚举只在显式调用时执行。该能力依赖可选的 `il2cpp_image_get_class_count` 和 `il2cpp_image_get_class` exports；缺失时返回 `missing_api`，不会影响普通类查询、调用或 Hook。它不扫描 GC 堆或读取任意托管对象。

如果托管参数类型暂时未知，可用 `method<Return, Args...>(name, parameter_count)` 按名称和参数数目查找；同名同参数数目的方法仍会返回歧义错误。实例方法可用 `hook_instance(...)` 安装 Hook，替换函数的第一个参数是 `Il2CppObject* this`。`Runtime::klass_of(instance)` 可取得对象的实际类；`fields()` 返回该类的字段元数据，`object_field(name)` 用于读取对象引用字段，`boxed_field(name)` 配合 `set_boxed(...)` 将已构造的装箱值写入值类型字段。`new_object()` 按类分配托管对象，可再通过构造函数初始化。

带类型的 Hook 替换函数会接收托管方法对应的原生参数。需要转发到原方法时，在替换函数中调用 `HookHandle::original()`。Hook 生效期间必须保留其句柄；`remove()` 可显式调用且可重复调用，句柄析构时也会移除仍生效的 Hook。析构时无法向调用方报告移除错误；需要处理错误时应显式调用 `remove()`。

需要长期持有 Hook 时可用 `HookSlot<Return, Args...>`：它串行化 `install()` 与 `remove()`，并在安装返回前发布原函数入口，避免替换函数读到尚未就绪的跳板。`install(installer)` 幂等，`original()` 是单次原子读取，`remove()` 可重复调用。

**替换函数必须自行判空。** Gum 可能在 `install()` 返回前就启用替换函数，移除成功后入口也会被清空；这两种情况下 `original()` 都返回 `nullptr`，此时应直接返回，不要通过空指针转发。

```cpp
#include "il2cpp_native_bridge/hook_slot.hpp"

INB::HookSlot<void, float> time_scale_slot;

void set_time_scale(float value) {
    if (auto original = time_scale_slot.original()) original(value);
}

(void) time_scale_slot.install([&] { return setter.hook(backend, set_time_scale); });
```

`make_frida_gum_hook_backend()` 会初始化已链接的 Frida Gum C 运行时（只初始化一次），并返回使用 Gum `Interceptor.replace` 和 `Interceptor.revert` 的 `HookBackend`；若无法取得 Gum interceptor 则返回空指针。`with_frida_gum_hook_backend(action)` 会统一创建并检查 backend，只在 backend 可用时调用 action，backend 不可用时返回 `hook_backend_unavailable`。这是原生 C 库依赖，不使用 Gadget、frida-core、GumJS 或 JavaScript 运行时。如果使用方只需要查询、字段访问和方法调用，可以不构建 Gum 构建目标；调用 Hook 时必须链接 `il2cpp_native_bridge_frida_gum`。

## 运行时状态与错误

调用 `Runtime::snapshot()` 查看运行时就绪状态和能力检测结果。如果 `libil2cpp.so` 可能在上次检查后才加载，请调用 `Runtime::refresh()`。各项操作返回 `Result<T>`，读取结果前应先检查状态。缺失的导出函数、运行时未就绪、成员不存在、重载不唯一、实例无效、不支持的方法入口以及 Gum Hook 失败，都会作为明确错误返回。

如果目标运行时缺少一个已知 IL2CPP 导出，可在首次调用 `Runtime::current()` 前调用 `Runtime::set_export_override("il2cpp_symbol", address)` 提供回退地址。只有模块符号查找失败时才使用该地址；运行时进入 `ready` 后不再接受配置。调用方需要确认回退地址属于当前进程中兼容的 IL2CPP 实现。

完整的 `set_timeScale` 调用流程见 [`examples/standalone`](../examples/standalone)。

## 新门面（`il2cpp_native_bridge/il2cpp.hpp`）

新门面把「等待运行时、附加线程、选后端、持有 Hook」这些步骤收进库内部，调用方只写一次 `perform`：

```cpp
#include "il2cpp_native_bridge/il2cpp.hpp"

using namespace INB;

// 替换回调必须是普通函数（无捕获）。
void on_set_time_scale(ReplacementCall<void, float>& call, float value) {
    call.original(value * 2.0f);
}

Il2Cpp::perform([] {
    auto time = Il2Cpp::domain()
                        .assembly("UnityEngine.CoreModule")
                        .image()
                        .klass("UnityEngine.Time");

    time.method("setTimeScale", 1).invoke<void, float>(0.5f);
    float observed = time.method("getTimeScale", 0).invoke<float>();

    auto setter = time.method("setTimeScale", 1);
    setter.implementation(&on_set_time_scale);   // 安装托管方法替换
    setter.revert();                             // 恢复原实现
});
```

要点：

- **作用域**：`Il2Cpp::perform` 的动作在库的协调线程上执行，动作内自动处于访问作用域；
  替换回调内同样自动建立作用域，可以直接继续调用 `Il2Cpp::domain()`。
- **异步**：`perform` 只表示「提交被接受」，完成通过 completion 回调送达；不要在 Unity 主线程同步等待。
- **重载**：常用 `perform(action)` 不传结果与诊断参数，失败由默认诊断报告；需要观察最终结果或等待阈值时用 `perform(action, completion, options)`。
- **对象链**：`domain → assembly → image → klass → method`，类解析限定在选定 image 内，
  因此同名类在不同程序集里不会互相污染。`method(name, arity)` 返回候选集，签名不唯一时用 `overload(...)` 或 `typed<R, Args...>()` 收窄。
- **错误**：第一个失败环节抛 `BridgeError`，动作边界转成 `Result<void>`；诊断行格式为
  `[il2cpp-native-bridge] stage=… path=… category=… message=…`。
- **不支持即拒绝**：泛型方法、无类型名映射的 C++ 类型在绑定期拒绝；缺少必要 IL2CPP
  导出时报 `missing_api`，不会退回猜测。
- **byref/ref/out**：`invoke` 与 `typed` 仍拒绝。`implementation` 允许用 C++ 指针载体绑定，
  条件是该指针有托管类型名映射且名字精确等于去掉 `&` 的托管参数名（例如 `bool* → System.Boolean`
  对应 `System.Boolean&`）；native ABI 本就按地址透传，其余不匹配情况仍在绑定期拒绝。

### 部署要求

`implementation` 首次调用时会读取 `UnityEngine.Application.get_unityVersion` 来选择 ABI profile。
IL2CPP 只为**可达**方法生成代码，因此宿主必须保留该 getter（托管侧引用一次，或在 `link.xml` 里保留），
否则替换会以 `not_found` 被拒绝，而查询与 `invoke` 不受影响。

### 支持范围

profile 按 family 与版本两级判定：`modern-static`（2018.3 起）与 `legacy-static-slot`（5.3–2018.2）。
只有列在 `kVerifiedVersions` 中、且通过运行时验收的版本是 `approved`；同 family 的其他版本是 `candidate`
（可安装但不声称支持）；legacy family 无生成源码证据，保持 `rejected`。
