#pragma once

// 与固定 Frida Gum 17.23.3 保持同形的替身声明；只覆盖新 provider 用到的部分。
// 真实 devkit 的同名符号见 third_party/frida-gum/17.23.3/android-arm64/frida-gum.h。

#ifdef __cplusplus
extern "C" {
#endif

typedef void* gpointer;
typedef int gboolean;

typedef struct _GumInterceptor {
    int unused;
} GumInterceptor;

typedef enum {
    GUM_REPLACE_OK = 0,
    GUM_REPLACE_INVALID_INSTRUCTION = -1,
    GUM_REPLACE_WRONG_SIGNATURE = -2,
    GUM_REPLACE_ALREADY_REPLACED = -3,
    GUM_REPLACE_POLICY_VIOLATION = -4,
    GUM_REPLACE_WRONG_TYPE = -5,
} GumReplaceReturn;

typedef struct _GumInterceptorOptions {
    int scratch_register;
    int scenario;
    int relocation_policy;
} GumInterceptorOptions;

typedef struct _GumReplaceOptions {
    GumInterceptorOptions instrumentation;
    gpointer replacement_data;
} GumReplaceOptions;

typedef struct _GumInvocationContext {
    gpointer function;
    gpointer user_data;
} GumInvocationContext;

void gum_init_embedded(void);
void gum_deinit_embedded(void);
GumInterceptor* gum_interceptor_obtain(void);
GumReplaceReturn gum_interceptor_replace(GumInterceptor* interceptor,
                                         gpointer target,
                                         gpointer replacement,
                                         gpointer* original_function,
                                         const GumReplaceOptions* options);
void gum_interceptor_revert(GumInterceptor* interceptor, gpointer target);
gboolean gum_interceptor_flush(GumInterceptor* interceptor);
gboolean gum_interceptor_flush_function(GumInterceptor* interceptor, const void* function_address);
GumInvocationContext* gum_interceptor_get_current_invocation(void);
gpointer gum_invocation_context_get_replacement_data(GumInvocationContext* context);
void g_object_unref(void* object);

#ifdef __cplusplus
}
#endif
