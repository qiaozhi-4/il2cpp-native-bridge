#include "frida-gum.h"
#include "fake_frida_gum_control.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace {

GumInterceptor interceptor;
std::atomic_int init_count{};
std::atomic_int replace_count{};
std::atomic_int revert_count{};
std::atomic_int unref_count{};
std::atomic_bool reject_replace{};
std::atomic_bool flush_failure{};
std::atomic_bool in_flight{};
std::atomic<fake_frida_gum::InstallHook> install_hook{};
std::atomic<void*> target_seen{};
std::atomic<void*> replacement_seen{};
std::atomic<void*> replacement_data_seen{};
int original_function_token{};

/// 目标 → 替换入口的极简表；容量必须覆盖 managed 预算用例的 256 个槽位。
struct Entry {
    void* target{};
    void* replacement{};
    void* data{};
    void* original{};
};
std::array<Entry, 320> entries{};
std::atomic<std::size_t> entry_count{};

/// 当前线程正在模拟的调用；真实 Gum 也按线程暴露当前 invocation。
thread_local GumInvocationContext* current_context{};
thread_local GumInvocationContext context_storage{};

Entry* find_entry(void* target) {
    const auto count = entry_count.load();
    for (std::size_t index = 0; index < count; ++index) {
        if (entries[index].target == target) return &entries[index];
    }
    return nullptr;
}

} /// namespace

extern "C" void gum_init_embedded(void) {
    ++init_count;
}

extern "C" void gum_deinit_embedded(void) {}

extern "C" GumInterceptor* gum_interceptor_obtain(void) {
    return &interceptor;
}

extern "C" GumReplaceReturn gum_interceptor_replace(GumInterceptor*,
                                                    gpointer target,
                                                    gpointer replacement,
                                                    gpointer* original_function,
                                                    const GumReplaceOptions* options) {
    ++replace_count;
    target_seen.store(target);
    replacement_seen.store(replacement);
    replacement_data_seen.store(options == nullptr ? nullptr : options->replacement_data);
    if (reject_replace.exchange(false)) return GUM_REPLACE_INVALID_INSTRUCTION;

    Entry* entry = find_entry(target);
    if (entry == nullptr) {
        const auto index = entry_count.load();
        if (index >= entries.size()) return GUM_REPLACE_POLICY_VIOLATION;
        entries[index] = Entry{target, replacement, options == nullptr ? nullptr : options->replacement_data, nullptr};
        entry_count.store(index + 1);
        entry = &entries[index];
    } else {
        entry->replacement = replacement;
        entry->data = options == nullptr ? nullptr : options->replacement_data;
    }

    /// 登记完成后、返回之前触发安装钩子：模拟早期回调窗口。
    if (auto hook = install_hook.load()) hook(target, replacement, entry->data);

    *original_function = entry->original != nullptr ? entry->original : &original_function_token;
    return GUM_REPLACE_OK;
}

extern "C" void gum_interceptor_revert(GumInterceptor*, gpointer target) {
    ++revert_count;
    target_seen.store(target);
    if (Entry* entry = find_entry(target)) entry->replacement = nullptr;
}

extern "C" gboolean gum_interceptor_flush(GumInterceptor*) {
    if (flush_failure.load()) return 0;
    return in_flight.load() ? 0 : 1;
}

extern "C" gboolean gum_interceptor_flush_function(GumInterceptor*, const void*) {
    if (flush_failure.load()) return 0;
    return in_flight.load() ? 0 : 1;
}

extern "C" GumInvocationContext* gum_interceptor_get_current_invocation(void) {
    return current_context;
}

extern "C" gpointer gum_invocation_context_get_replacement_data(GumInvocationContext* context) {
    return context == nullptr ? nullptr : context->user_data;
}

extern "C" void g_object_unref(void*) {
    ++unref_count;
}

namespace fake_frida_gum {

ScopedInvocation::ScopedInvocation(void* target) {
    Entry* entry = find_entry(target);
    context_storage.function = target;
    context_storage.user_data = entry == nullptr ? nullptr : entry->data;
    current_context = &context_storage;
}

ScopedInvocation::~ScopedInvocation() { current_context = nullptr; }

void set_original(void* target, void* original) {
    Entry* entry = find_entry(target);
    if (entry == nullptr) {
        const auto index = entry_count.load();
        if (index >= entries.size()) return;
        entries[index] = Entry{target, nullptr, nullptr, original};
        entry_count.store(index + 1);
        return;
    }
    entry->original = original;
}

void* replacement_for(void* target) {
    Entry* entry = find_entry(target);
    return entry == nullptr ? nullptr : entry->replacement;
}

void reset() {
    init_count.store(0);
    replace_count.store(0);
    revert_count.store(0);
    unref_count.store(0);
    reject_replace.store(false);
    flush_failure.store(false);
    in_flight.store(false);
    install_hook.store(nullptr);
    target_seen.store(nullptr);
    replacement_seen.store(nullptr);
    replacement_data_seen.store(nullptr);
    entry_count.store(0);
    entries.fill(Entry{});
}

void reject_next_replace() { reject_replace.store(true); }
void fail_flush(bool fail) { flush_failure.store(fail); }
void set_in_flight(bool value) { in_flight.store(value); }
void set_install_hook(InstallHook hook) { install_hook.store(hook); }
int init_calls() { return init_count.load(); }
int replace_calls() { return replace_count.load(); }
int revert_calls() { return revert_count.load(); }
int unref_calls() { return unref_count.load(); }
void* last_target() { return target_seen.load(); }
void* last_replacement() { return replacement_seen.load(); }
void* last_replacement_data() { return replacement_data_seen.load(); }

} /// namespace fake_frida_gum
