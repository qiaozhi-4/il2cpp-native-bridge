#pragma once

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

void gum_init_embedded(void);
GumInterceptor* gum_interceptor_obtain(void);
GumReplaceReturn gum_interceptor_replace(GumInterceptor* interceptor,
                                         gpointer target,
                                         gpointer replacement,
                                         gpointer* original_function,
                                         const void* options);
void gum_interceptor_revert(GumInterceptor* interceptor, gpointer target);
gboolean gum_interceptor_flush_function(GumInterceptor* interceptor, const void* function_address);
void g_object_unref(void* object);

#ifdef __cplusplus
}
#endif
