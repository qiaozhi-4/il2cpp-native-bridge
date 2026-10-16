#include "frida-gum.h"
#include "fake_frida_gum_control.hpp"

#include <atomic>

namespace {

GumInterceptor interceptor;
std::atomic_int init_count{};
std::atomic_int replace_count{};
std::atomic_int revert_count{};
std::atomic_int unref_count{};
std::atomic_bool reject_replace{};
std::atomic_bool flush_failure{};
std::atomic<void*> target_seen{};
int original_function_token{};

} /// namespace

extern "C" void gum_init_embedded(void) {
    ++init_count;
}

extern "C" GumInterceptor* gum_interceptor_obtain(void) {
    return &interceptor;
}

extern "C" GumReplaceReturn gum_interceptor_replace(GumInterceptor*,
                                                       gpointer target,
                                                       gpointer,
                                                       gpointer* original_function,
                                                       const void*) {
    ++replace_count;
    target_seen.store(target);
    if (reject_replace.exchange(false)) return GUM_REPLACE_INVALID_INSTRUCTION;
    *original_function = &original_function_token;
    return GUM_REPLACE_OK;
}

extern "C" void gum_interceptor_revert(GumInterceptor*, gpointer target) {
    ++revert_count;
    target_seen.store(target);
}

extern "C" gboolean gum_interceptor_flush_function(GumInterceptor*, const void*) {
    return flush_failure.load() ? 0 : 1;
}

extern "C" void g_object_unref(void*) {
    ++unref_count;
}

namespace fake_frida_gum {

void reset() {
    init_count.store(0);
    replace_count.store(0);
    revert_count.store(0);
    unref_count.store(0);
    reject_replace.store(false);
    flush_failure.store(false);
    target_seen.store(nullptr);
}

void reject_next_replace() { reject_replace.store(true); }
void fail_flush(bool fail) { flush_failure.store(fail); }
int init_calls() { return init_count.load(); }
int replace_calls() { return replace_count.load(); }
int revert_calls() { return revert_count.load(); }
int unref_calls() { return unref_count.load(); }
void* last_target() { return target_seen.load(); }

} /// namespace fake_frida_gum
