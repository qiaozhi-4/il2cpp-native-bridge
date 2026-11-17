#pragma once

/**
 * il2cpp-native-bridge 内部日志。
 *
 * 风格参考 BNM：用编译期开关控制，关闭时宏展开为空操作，不引入 android/log.h，
 * 让库的 host 构建与单测保持干净。日志只写 logcat，不依赖宿主 Logger，也不进 ImGui 面板。
 *
 * 仅在同时满足 INB_ENABLE_LOGCAT（由 CMake 在 Android Debug 构建注入）与 __ANDROID__ 时启用；
 * 双重条件确保即使在 host 上误定义了开关，也不会因缺少 android/log.h 而编译失败。
 */
#if defined(INB_ENABLE_LOGCAT) && defined(__ANDROID__)

#include <android/log.h>

#define INB_LOG_TAG "INB"

#define INB_LOGE(...) ((void) __android_log_print(ANDROID_LOG_ERROR, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGW(...) ((void) __android_log_print(ANDROID_LOG_WARN, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGI(...) ((void) __android_log_print(ANDROID_LOG_INFO, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGD(...) ((void) __android_log_print(ANDROID_LOG_DEBUG, INB_LOG_TAG, __VA_ARGS__))

#else

#define INB_LOGE(...) ((void) 0)
#define INB_LOGW(...) ((void) 0)
#define INB_LOGI(...) ((void) 0)
#define INB_LOGD(...) ((void) 0)

#endif
