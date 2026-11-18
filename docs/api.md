# C++ 接入 API

库提供两层公开接口，共享同一个运行时 context：

- **统一门面**（`il2cpp_native_bridge/il2cpp.hpp`）：一次 `Il2Cpp::perform` 完成等待就绪、附加线程、查询、调用与替换，库内部处理作用域、后端与生命周期。下文先介绍这一层。
- **底层 API**（`bridge.hpp` 等头文件）：显式编排线程附加、等待策略、Hook 保活与后端选择。见[底层 API](#底层-api)。

C++ 命名空间统一为 `INB`。头文件目录与 CMake 目标名保持 `il2cpp_native_bridge`。所有并行头文件均可单独包含。

## 统一门面（`il2cpp.hpp`）

### 提交动作

`Il2Cpp::perform` 把所有动作提交到库的协调线程，异步、FIFO、串行执行。返回值只表示提交是否被接受，不代表动作已执行或替换已安装。

```cpp
#include "il2cpp_native_bridge/il2cpp.hpp"

// 替换回调是无捕获的普通函数；签名由被替换方法的返回类型与参数决定。
void on_set_time_scale(INB::ReplacementCall<void, float>& call, float value) {
    if (value > 0.0f) call.original(value);  // 保留实例与隐藏 MethodInfo 转发原实现
}

auto submitted = INB::Il2Cpp::perform([] {
    auto time = INB::Il2Cpp::domain()
                        .assembly("UnityEngine.CoreModule")
                        .image()
                        .klass("UnityEngine.Time");

    auto setter = time.method("set_timeScale", 1);
    auto getter = time.method("get_timeScale", 0);

    setter.invoke<void, float>(0.5f);
    (void) getter.invoke<float>();

    setter.implementation(&on_set_time_scale);  // 安装托管方法替换
    setter.revert();                            // 恢复原实现
});
```

两个重载：

- `perform(action)`：等价于三参重载传空 completion 与默认选项；失败由默认诊断报告。
- `perform(action, completion, options)`：需要观察结果或等待阈值时使用。

动作只能返回 `void` 或 `INB::Result<void>`，其他返回类型在编译期拒绝。动作抛出的 `BridgeError` 或标准异常在工作线程边界转换为失败的 `Result<void>`，不会传播到提交方。`completion` 在协调线程执行，但已离开访问作用域，且至多调用一次；它抛出的异常只被报告，不改变结果或重跑动作。返回值在队列已满、执行器停机或参数非法时返回失败。

`PerformOptions` 控制等待诊断：`diagnostic_after`（默认 10 秒）为从提交开始的等待阈值，`on_wait` 在超过阈值时最多触发一次，运行于协调线程，用于读取 `RuntimeSnapshot`；`diagnostic_after` 为 0 关闭通知，为负在提交期被拒绝。

### 访问作用域

动作在库的协调线程上执行，期间自动处于运行时访问作用域，可直接调用 `Il2Cpp::domain()`。替换回调执行时也会建立同样的作用域，因此回调内可以继续调用 `Il2Cpp::domain()`。作用域建立失败（例如运行时已不可用）时，回调内的作用域降级为空操作，异常不会越过 native 栈。在 `perform` 或替换回调之外调用门面 API 会抛 `BridgeError`（`scope_violation`）。

### 对象链查询

`domain → assembly → image → klass → method` 逐层收窄，类解析限定在选定的 image 内，同名类在不同程序集不会互相污染。已解析的名称可以带出作用域，只有实际读取运行时元数据的操作要求在作用域内。每次操作核对 context 与 generation，旧代际的句柄明确失败（`stale_handle`）而不是返回过期数据。

- `Domain::assembly(name)`：按程序集名选择唯一程序集，容忍一次 `.dll` 后缀；无结果或同名歧义抛错。
- `Assembly::image()`：取得该程序集对应的真实 image。
- `Image::klass(full_name)`：只在该 image 内按含命名空间的完整类名查找。
- `Class::method(name)`、`Class::method(name, managed_parameter_count)`：返回候选选择对象。0 个候选立即抛错；多个候选继续用下面的方法收窄。参数个数是托管参数个数，不计 `this` 与隐藏 `MethodInfo`。
- `Method::overload(parameter_types)`：按参数类型名继续缩小候选；参数类型名与托管签名逐位相等才匹配。

对于版本分界：Unity 6 把类型分布到多个模块，用底层全 `Runtime::klass("UnityEngine.Time")` 会报多程序集歧义；门面要求显式 `assembly → image → klass`，不受影响。

### 调用

方法解析后可直接调用，也可用 `typed<R, Args...>()` 先得到可复用的绑定：

```cpp
auto setter = time.method("set_timeScale", 1);

setter.invoke<void, float>(0.5f);                 // 静态方法调用
float v = getter.invoke<float>();                 // 返回值按值拆箱

auto bound = setter.typed<void, float>();         // 校验一次静态签名的绑定
bound.invoke(1.0f);
// 实例方法使用 invoke_on(instance, args...)，实例须与声明类兼容且非静态。
```

参数是按值还是按指针传递，由绑定的参数类别决定：引用类型传对象指针值，基础类型传值地址。`invoke` 会核对每个非空引用实参的赋值兼容性。C++ 类型到托管类型名的映射见[类型映射](#类型映射)。

`invoke` 与 `typed` 拒绝 byref/ref/out 签名；带类型名映射的指针载体在替换绑定里可按 byref 处理，见下节。

### 托管方法替换

`implementation` 安装托管方法替换，由库强拥有记录，不返回需要保活的句柄。回调形式固定为：

```cpp
Return on_method(INB::ReplacementCall<Return, Args...>& call, Args... args);
```

- 回调必须是无捕获的普通函数指针。实例方法的 `this` 不在 `Args` 里，用 `call.self()` 取得（静态方法为 `nullptr`）。
- `call.original(args...)` 转发原实现，自动补上当前实例与隐藏 `MethodInfo`，可多次显式调用。
- 回调抛出的异常被隔离在 native 边界：库按节流频率报告诊断，并返回默认值，不自动补调用原实现。

```cpp
void on_set_time_scale(INB::ReplacementCall<void, float>& call, float value) {
    if (auto* self = call.self()) { /* 实例方法可用 self */ }
    call.original(value);
}
```

`revert()` 恢复当前 managed target，成功后再调用走原实现；重复恢复成功。`replacement_state()` 返回当前替换状态：`not_installed`、`installing`、`active`、`reverting`、`draining`。恢复发出但仍有调用停留在旧入口时抛 `restore_pending`，可在调用排空后重试。同一地址同时只能有一个所有者：已被底层 Hook 或另一替换占用时报 `owner_conflict`；名额上限为 256，耗尽时报 `resource_exhausted`。

**byref/ref/out 的绑定**：`implementation` 允许用 C++ 指针载体绑定 byref 参数，条件是该指针有托管类型名映射且基础名精确等于去掉 `&` 的托管参数名。库已为基础值类型内置指针载体映射，例如 `bool*` → `System.Boolean`（对应托管参数 `System.Boolean&`）、`std::int32_t*` → `System.Int32`（对应 `System.Int32&`）；其余类型需自行特化。native ABI 本就按地址透传，不匹配情况在绑定期拒绝。

**部署前提**：首次调用 `implementation` 会读取 `UnityEngine.Application.get_unityVersion` 选择 ABI profile。IL2CPP 只为可达方法生成代码，宿主必须保留该 getter（托管侧引用一次，或在 `link.xml` 中保留），否则替换以 `not_found` 被拒绝；查询与 `invoke` 不受影响。

### 类型映射

`INB::ManagedTypeName<T>` 把 C++ 类型映射到托管类型名，无映射的类型无法参与方法/字段查询，相关模板在编译期拒绝。特化它可扩展新类型，需提供静态成员 `value`：

```cpp
namespace INB {
template <> struct ManagedTypeName<MyStruct> {
    static constexpr std::string_view value = "My.Namespace.MyStruct";
};
}
```

内建映射覆盖 `void`、`bool`、各宽度整型、`float`、`double`、`Il2CppObject*`（`System.Object`）与 `Il2CppString*`（`System.String`），以及上述基础值类型的指针载体（用于 byref/ref/out）。`managed_type_name<T>` 变量模板免去书写 `ManagedTypeName<remove_cvref_t<T>>::value`。自定义类型作为指针载体时仍需自行特化，见上节。

库提供 `INB::UnityVector2`，其内存布局与 `UnityEngine.Vector2` 一致（两个连续 `float`），可直接用于按值传参与返回，并已带好类型名映射。布局带有静态断言，不要改动字段顺序或数量。

### 错误与诊断

门面在第一个失败环节抛 `INB::BridgeError`，动作边界转成 `Result<void>`。`BridgeError::error()` 返回结构化 `Error`：

- `code`：`ErrorCode` 分类，见下文。
- `stage`：`ErrorStage` 阶段（`submit`/`readiness`/`scope`/`lookup`/`bind`/`invoke`/`replacement`/`restore`/`completion`）。
- `path`：域/程序集/类/方法的定位路径，不含对象内容或设备信息。
- `message`：面向诊断的描述，不保证跨版本稳定，不用于逻辑判断。

失败报告格式为 `[il2cpp-native-bridge] stage=… path=… category=… message=…`。Android Debug 构建可开启 `INB_ENABLE_LOGCAT`，在失败的唯一出口以 `INB` 标签写入 logcat。

同一开关还会输出 Debug 轨迹，标签同为 `INB`（`logcat -s INB`）：

- 模块与导出：`IL2CPP module: …` 与逐符号的 `export il2cpp_class_from_name: 0x… (+0x…)`；只在首次解析导出表时打印，缺失的必需导出记 Error、可选导出记 Debug，末尾附 `exports resolved: …` 汇总。
- 就绪迁移：`runtime waiting_for_module / initializing / ready / unsupported`，每个阶段只打印一次，`ready` 附带代际与已加载程序集数量。
- 首次解析：`class …`、`method …::…`、`field …::…` 与门面的 `bind …`。同一查询在进程内只打印一次，成功与失败都只记一次，因此逐帧查询不会刷屏。
- Hook：`hook installed`、`replacement installed`、`hook removed`、`replacement reverted`。
- 门面生命周期：`perform#N queued`、`perform#N start after N ms`、`perform#N done after N ms`，用于区分「卡在等待就绪」与「动作本身耗时」。

日志在编译期关闭时展开为空操作：既不生成格式串，也不做计时或去重查表，Release 构建没有残留开销。

`ErrorCode` 中与门面相关的分类：`scope_violation`（作用域外执行运行时操作）、`resource_exhausted`（有界资源耗尽）、`restore_pending`（恢复未确认，可稍后重试）、`owner_conflict`（地址由其他所有者管理）。底层共用的分类包括 `runtime_not_ready`、`unsupported_runtime`、`missing_api`、`not_found`、`ambiguous_match`、`type_mismatch`、`invalid_instance`、`invocation_failed`、`managed_exception`、`entry_point_unavailable`、`hook_backend_unavailable`、`hook_install_failed`、`stale_handle`、`invalid_argument`、`internal_error`。调用方按 `code` 决定是等待重试还是改用其他方式。

### 支持范围

ABI profile 按 family 与版本两级判定。`modern-static`（2018.3 起且已验证的版本）为 `approved`；同 family 其他版本为 `candidate`（可安装但不声称支持）；`legacy-static-slot`（5.3–2018.2）无生成源码证据，保持 `rejected`。形态相同不等于已在某版本上验过。细节见 [`compatibility.md`](compatibility.md)。

---

## 底层 API

底层接口以 `INB::Result<T>` 返回错误，读取结果前先检查状态。`Result<T>` 用 `has_value()` 或布尔转换判断，成功值用 `value()`/`take_value()`，失败用 `error()`；`Result<void>` 只表示成功或失败。

### 运行时生命周期

```cpp
#include "il2cpp_native_bridge/bridge.hpp"

using namespace INB;

auto runtime = Runtime::current();
auto snapshot = runtime.snapshot();     // 就绪状态、能力、缺失导出、last_error
if (snapshot.state != RuntimeState::ready) { /* 等待或诊断 */ }
```

- `Runtime::current()` 获取进程内唯一实例，并尝试刷新就绪状态；未就绪时仍返回实例。
- `Runtime::snapshot()`、`Runtime::refresh()`：查看状态与主动刷新。`RuntimeState` 取 `waiting_for_module`、`initializing`、`ready`、`unsupported`。
- `Runtime::with_attached_thread(action)`：同步检查运行时就绪，并在回调期间确保当前线程已附加；仅当本次调用完成附加时才在回调结束后分离。它不等待运行时、不切到 Unity 主线程、不安排重试。需要更长的附加生命周期时用 `ThreadAttachment::attach(runtime)`。
- `Runtime::perform_when_ready(action, completion, options)`：在后台线程轮询模块、`il2cpp_get_corlib` 与程序集状态，就绪后附加线程并只执行一次 action。等待超过 `options.timeout` 时最多调用一次 `options.on_timeout`，然后继续等待。`RuntimeWaitOptions` 控制轮询间隔（默认 100 ms）、超时（默认 10 s）与超时回调。
- `Runtime::set_symbol_lookup(module, lookup, is_ready)`：注册宿主自定义的导出符号解析器，替代默认模块扫描。只设置解析器、不做等待；必须在任何运行时操作前调用，参数为空或运行时已进入 `ready` 时记录诊断并终止进程。以完全相同的三个参数重复调用无副作用。
- `Runtime::set_export_override(symbol_name, address)`：为单个导出注册地址覆盖，仅在符号查找失败时使用，且只能在运行时就绪前调用。调用方需确认地址属于兼容的 IL2CPP 实现。

### 类、方法与字段查询

底层以延迟解析的 `ClassQuery` 为核心：构造时不报错，通过 `found()`/`error()` 暴露结果。

```cpp
auto time = runtime.klass("UnityEngine.Time").in_assembly("UnityEngine.CoreModule");
if (!time.found()) { /* 读取 time.error() */ }

auto setter = time.method<void, float>("set_timeScale");      // 精确签名
auto by_arity = time.method<void, float>("set_timeScale", 1); // 按名字与参数个数
auto field = time.field<int>("someField");

setter.invoke(1.0f);                    // 静态方法
// setter.invoke_on(instance, 1.0f);    // 实例方法
```

- `Runtime::klass(name)` 在全域搜索并要求结果唯一；`in_assembly(name)` 缩小范围。`class_by_name` 直接返回原生句柄。
- `Runtime::klass_of(instance)` 查询对象实际所属的类；`Runtime::base_class(query)` 返回直接基类查询，用于沿继承链查找 IL2CPP 不列出的继承方法；`Runtime::generic_class(name, type_arguments, assembly_name)` 构造封闭泛型类，依赖 reflection 相关可选导出，缺失时返回失败结果。
- `Runtime::classes(filter)` 按完整类名子串枚举已加载的类；空字符串枚举全部。它依赖可选的 `il2cpp_image_get_class_count`/`il2cpp_image_get_class` 导出，缺失时返回 `missing_api`。`ClassQuery::methods()` 返回方法名、返回类型、参数类型与静态标记；`fields()` 返回字段句柄。枚举只在显式调用时执行，不扫描 GC 堆或读取任意对象内容。
- `ClassQuery::field<T>(name)` 提供类型化读写：`get(instance)`、`set(value, instance)`、`set_boxed(boxed, instance)`。`object_field(name)` 用于不核对类型的引用字段，`boxed_field(name)` 用于以装箱形式写入值类型字段。`new_object()` 按类分配实例。
- `method<Return, Args...>(name)` 按精确托管签名选择；找不到或不唯一都返回错误，不静默选某个重载。同名同参数数目仍可用 `method(name, count)` 的 arity 形式，歧义时按错误返回。
- 静态字段传 `instance = nullptr`；实例字段的 `instance` 必须与声明类兼容。

### 数组、字典与字符串

字符串转换、数组遍历、`List<T>` 遍历与字典遍历的完整 API、成员表和示例见独立的 [集合与字符串工具](collections.md)。底层增加的能力包括：

- `Runtime::array_length(array)`：优先用 `il2cpp_array_length`，导出缺失时回退 `System.Array.get_Length`。
- `Runtime::unbox(boxed)`：返回装箱值的数据指针，供按字节读取。
- `Runtime::base_class(query)`：返回直接基类查询，用于沿继承链查找 IL2CPP 不列出的继承方法，依赖 `il2cpp_class_get_parent`。
- `Runtime::string_to_utf8/string_to_utf16` 与 `string_from_utf8/string_from_utf16`：托管字符串与 C++ 文本的双向转换。

`collections.hpp` 提供 `ArrayView<T>`、`ListView<T>`、`DictionaryView<K,V>` 三组只读视图，以及上述字符串方法；`managed_types.hpp` 在其上提供抛 `BridgeError` 的 `ManagedString`、`ManagedList<T>`、`ManagedDictionary<K,V>` 便捷包装。它们共用同一个 `Runtime` context，可在 `Il2Cpp::perform` 作用域内或低层 `Runtime::current()` 代码里使用。

### Hook 与后端

底层 Hook 用 `HookBackend` 抽象原生替换能力，`MethodQuery::hook`/`hook_instance` 安装并返回一个需要保活的 `HookHandle`：

```cpp
#include <optional>
#include "il2cpp_native_bridge/frida_gum_hook_backend.hpp"

std::optional<INB::HookHandle<void, float>> handle;

auto result = runtime.with_attached_thread([&]() -> INB::Result<void> {
    return INB::with_frida_gum_hook_backend([&](const auto& backend) -> INB::Result<void> {
        auto time = runtime.klass("UnityEngine.Time");
        auto setter = time.method<void, float>("set_timeScale");
        auto installed = setter.hook(backend, replacement);
        if (!installed) return INB::Result<void>::failure(installed.error());
        handle.emplace(installed.take_value());
        return INB::Result<void>::success();
    });
});
```

- `handle` 必须在 Hook 存续期间保持有效。`remove()` 可重复调用；句柄析构时也会移除仍生效的 Hook，但析构期无法报告错误，需要处理错误时显式调用 `remove()`。
- `HookHandle::original()` 返回原函数跳板；Hook 已移除或句柄为空时返回 `nullptr`。替换函数必须自行判空，移除成功后入口也会被清空。
- `hook_instance` 安装实例方法替换，替换函数首参为 `Il2CppObject* this`。
- `make_frida_gum_hook_backend()` 初始化已链接的 Frida Gum C 运行时（进程内一次）并返回 backend；`with_frida_gum_hook_backend(action)` 只在 backend 可用时调用 action，否则返回 `hook_backend_unavailable`。`perform_when_ready_with_frida_gum_hook_backend(runtime, action, completion, options)` 把等待运行时、附加线程与创建 backend 组合起来，就绪后 action 至多执行一次。
- 只做查询、字段访问和调用的消费方可以关闭 Gum 构建目标；安装 Hook 时需要链接 `il2cpp_native_bridge_frida_gum`。

需要长期持有时用 `HookSlot<Return, Args...>`：它串行化安装与移除，在安装返回前发布原函数入口，避免替换函数读到尚未就绪的跳板。`install(installer)` 幂等，`original()` 是一次原子读取，`remove()` 可重复调用。

```cpp
#include "il2cpp_native_bridge/hook_slot.hpp"

INB::HookSlot<void, float> slot;

void set_time_scale(float value) {
    if (auto original = slot.original()) original(value);
}

(void) slot.install([&] { return setter.hook(backend, set_time_scale); });
```

替换函数仍须自行判空：Gum 可能在 `install()` 返回前就启用替换函数，此时 `original()` 返回 `nullptr`；移除成功后同样清空入口。

完整可编译示例见 [`../examples/standalone`](../examples/standalone)。
