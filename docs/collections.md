# 集合与字符串工具

`il2cpp_native_bridge/collections.hpp` 提供托管字符串、数组、`List<T>` 与字典的 C++ 侧工具。它们不引入新的运行时层，和 `bridge.hpp` 共用同一个 `Runtime` context；可以在 `Il2Cpp::perform` 的作用域内使用，也可以用 `Runtime::current()` 在低层代码里使用。

所有可能失败的操作都返回 `INB::Result<T>`：先检查布尔值或 `has_value()`，失败时读 `error()`。视图的 `bind` 不抛异常，通过 `found()`/`error()` 暴露解析结果。

这里有两层接口：

- `collections.hpp`：`Result<T>` 底层视图，错误显式、不做语法糖。
- `managed_types.hpp`：在其上提供 `ManagedString`、`ManagedList<T>`、`ManagedDictionary<K,V>` 便捷包装，失败抛 `BridgeError`，因此适合直接写在 `Il2Cpp::perform` 的 lambda 里——异常会在任务边界统一转换为 `Result<void>`。

## 便捷包装层（推荐日常使用）

```cpp
#include "il2cpp_native_bridge/managed_types.hpp"

(void) INB::Il2Cpp::perform([&] {
    auto runtime = INB::Runtime::current();

    // 字符串：像 C++ 字符串一样读取、打印、拼接
    auto text = INB::ManagedString::from_utf8(runtime, "敌人: 3");
    LOGI("%s", text.utf8().c_str());
    LOGI("length=%zu empty=%d", text.size(), text.empty());

    text += " 已击杀";                 // 创建新的 System.String 并重新绑定
    LOGI("%s", text.utf8().c_str());

    // 列表：数量、下标访问、range-for、转 vector
    auto list = INB::ManagedList<Il2CppObject*>::adopt(runtime, enemies);
    LOGI("count=%zu", list.size());
    auto first = list[0];
    for (auto enemy : list) {
        // 处理每个敌人
    }
    auto snapshot = list.to_vector();

    // 字典：数量、range-for 遍历每个 kv、转 vector
    auto dict = INB::ManagedDictionary<Il2CppString*, Il2CppObject*>::adopt(runtime, table);
    LOGI("pairs=%zu", dict.size());
    for (auto [key, value] : dict) {
        LOGI("key=%s value=%p", key.utf8().c_str(), (void*) value);
    }
    auto pairs = dict.to_vector();
});
```

便捷包装的成员：

| 类型 | 成员 |
|---|---|
| `ManagedString` | `adopt`、`from_utf8`、`from_utf16`、`utf8`、`utf16`、`size`/`length`、`empty`、`append`、`concat`、`operator+=`、`operator+`、`==`/`!=`、`operator<<`、`native` |
| `ManagedList<T>` | `adopt`、`bind`（Result 版）、`size`/`count`、`empty`、`operator[]`、`at`、`try_at`、`for_each`、`to_vector`、`begin`/`end`、`native` |
| `ManagedDictionary<K,V>` | `adopt`、`bind`（Result 版）、`size`/`count`、`empty`、`for_each`、`to_vector`、`begin`/`end`、`native` |

**字符串不可变**：`System.String` 没有原地修改。`text += "..."` 会用拼接后的内容创建新的 `System.String`，并让 `text` 重新指向它；原来的托管字符串不变。真正的原地追加需要 `System.Text.StringBuilder`，当前未提供。

`ManagedList<T>::at` / `operator[]` 越界时抛 `BridgeError`；`try_at` 返回 `Result<T>`。`ManagedDictionary` 的 range-for 会先把 kv 物化到 `std::vector`，因此遍历期间字典变化不会影响当前循环。

## 字符串（底层 Result 接口）

`Runtime` 上提供四个字符串方法：

| 方法 | 返回 | 说明 |
|---|---|---|
| `string_to_utf8(Il2CppString*)` | `Result<std::string>` | 读取并转成 UTF-8 |
| `string_to_utf16(Il2CppString*)` | `Result<std::u16string>` | 按原 UTF-16 内容读取 |
| `string_from_utf8(std::string_view)` | `Result<Il2CppString*>` | 分配托管字符串，内嵌空字符不截断 |
| `string_from_utf16(std::u16string_view)` | `Result<Il2CppString*>` | 分配托管字符串 |

转换走 `il2cpp_string_new_utf16`，不依赖当前区域设置；无效码点替换为 U+FFFD。创建依赖可选导出 `il2cpp_string_new_utf16`，读取依赖 `il2cpp_string_length`/`il2cpp_string_chars`。

```cpp
#include "il2cpp_native_bridge/collections.hpp"

auto runtime = INB::Runtime::current();

// C++ 文本 -> 托管字符串
auto managed = runtime.string_from_utf8("玩家: 明日方舟");
if (!managed) {
    LOGW("创建字符串失败: %s", managed.error().message.c_str());
    return;
}

// 托管字符串 -> C++ 文本
auto text = runtime.string_to_utf8(managed.value());
if (text) {
    LOGI("读回: %s", text.value().c_str());
}
```

## 数组：`ArrayView<T>`

公开成员：

- `bind(runtime, array)`：`Il2CppObject*` 与 `Il2CppArray*` 两种重载。
- `found()` / `error()` / `native()`
- `size()` → `Result<std::size_t>`
- `at(index)` → `Result<T>`，越界报错
- `for_each(callback)` → `Result<void>`，回调为 `void(T)` 或 `bool(T)`

元素访问统一走 `System.Array.GetValue(int)`：引用类型直接得到对象指针，值类型经装箱后按字节取出。长度优先用 `il2cpp_array_length`，导出缺失时回退 `System.Array.get_Length`。

```cpp
auto array = controller.method<Il2CppObject*>("get_allEnemies", 0).invoke();
auto view = INB::ArrayView<Il2CppObject*>::bind(runtime, array.value());
if (!view.found()) {
    LOGW("数组绑定失败: %s", view.error().message.c_str());
    return;
}

// 按下标读取
auto first = view.at(0);
if (first) LOGI("第一个敌人: %p", (void*) first.value());

// 遍历全部元素
(void) view.for_each([](Il2CppObject* enemy) {
    // 处理每个敌人
});

// 找到目标后提前结束
(void) view.for_each([](Il2CppObject* enemy) -> bool {
    return enemy != target_enemy;   // false 停止遍历
});
```

值类型数组同样适用：

```cpp
auto numbers = INB::ArrayView<std::int32_t>::bind(runtime, int_array);
(void) numbers.for_each([](std::int32_t value) {
    LOGI("value=%d", value);
});
```

## 列表：`ListView<T>`

公开成员与 `ArrayView<T>` 一致：`bind`、`found`、`error`、`native`、`size`、`at(index)`、`for_each(callback)`。

它从对象的具体类查找 `get_Count` 与 `get_Item(int)`，并沿继承链上溯，不依赖 `_items` 字段布局，也不需要调用方提供 `T` 的托管名。

```cpp
auto list_object = field.get(list_owner).value();   // 假设这是某个 List<Enemy>
auto list = INB::ListView<Il2CppObject*>::bind(runtime, list_object);
if (!list.found()) {
    LOGW("列表绑定失败: %s", list.error().message.c_str());
    return;
}

auto count = list.size();
LOGI("列表长度: %d", count ? (int) count.value() : -1);

(void) list.for_each([](Il2CppObject* enemy) {
    // 处理每个元素
});
```

`T` 也可以是基础值类型：

```cpp
auto ids = INB::ListView<std::int32_t>::bind(runtime, int_list);
(void) ids.for_each([](std::int32_t id) -> bool {
    if (id == 42) return false;     // 提前结束
    return true;
});
```

## 字典：`DictionaryView<K,V>`

公开成员：

- `bind(runtime, dictionary)`：不需要泛型实参名。
- `found()` / `error()` / `native()`
- `size()` → `Result<std::size_t>`，键值对数量
- `for_each(callback)` → `Result<void>`，回调为 `void(K,V)` 或 `bool(K,V)`

视图从对象的具体类查找 `get_Count`/`GetEnumerator`，再从枚举器具体类查找 `MoveNext`/`get_Key`/`get_Value`。IL2CPP 的 `class_get_methods` 不含继承成员，因此视图用 `Runtime::base_class` 逐级上溯；如果目标类只实现接口而没有直接声明这些方法，绑定会失败。上溯依赖可选导出 `il2cpp_class_get_parent`。

```cpp
auto dictionary = INB::DictionaryView<Il2CppString*, Il2CppObject*>::bind(runtime, dict_object);
if (!dictionary.found()) {
    LOGW("字典绑定失败: %s", dictionary.error().message.c_str());
    return;
}

auto count = dictionary.size();
LOGI("键值对数量: %d", count ? (int) count.value() : -1);

// 遍历每个 kv
(void) dictionary.for_each([&](Il2CppString* key, Il2CppObject* value) {
    auto key_text = runtime.string_to_utf8(key);
    if (key_text) LOGI("key=%s value=%p", key_text.value().c_str(), (void*) value);
});
```

值类型的键和值会经 `Runtime::unbox` 按字节取出：

```cpp
auto pairs = INB::DictionaryView<std::int32_t, std::int32_t>::bind(runtime, dict_object);
(void) pairs.for_each([](std::int32_t key, std::int32_t value) {
    LOGI("%d -> %d", key, value);
});
```

## 共同约定

- 回调在当前线程同步执行，允许捕获局部状态；**不需要构造托管 delegate**。
- 回调返回 `bool` 时，返回 `false` 提前结束；返回 `void` 时遍历全部。
- 视图只借用托管对象，不保活。在可能发生 GC 的地方由调用方保持对象引用。
- `DictionaryView` 目前只提供 `size()` 和 `for_each()`，没有 `at(key)`/`try_get`；需要按键取值时先用 `for_each` 过滤，或后续再扩展。
