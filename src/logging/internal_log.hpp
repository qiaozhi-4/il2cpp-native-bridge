#pragma once

/**
 * il2cpp-native-bridge 内部日志。
 *
 * 风格参考 BNM：用编译期开关控制，关闭时宏展开为空操作。日志只写 logcat，
 * 不依赖宿主 Logger，也不进 ImGui 面板。
 *
 * 开关由 INB_ENABLE_LOGCAT 单独控制，该宏只由 CMake 在 Android 构建注入；
 * 未定义时连 <android/log.h> 都不引入，日志宏全部展开为空操作。
 *
 * INB_LOG_ENABLED 是同一开关的 C++ 常量形式，供 `if constexpr` 使用：宏只管「调不调用
 * __android_log_print」，调用点周围的配套代码（去重查表、字符串拼接、计时）要靠它一起丢弃，
 * 否则关闭日志后这些逻辑仍会照常执行。
 */
#if defined(INB_ENABLE_LOGCAT)

#include <android/log.h>

#define INB_LOG_TAG "INB"

#define INB_LOG_ENABLED 1

#define INB_LOGE(...) ((void) __android_log_print(ANDROID_LOG_ERROR, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGW(...) ((void) __android_log_print(ANDROID_LOG_WARN, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGI(...) ((void) __android_log_print(ANDROID_LOG_INFO, INB_LOG_TAG, __VA_ARGS__))
#define INB_LOGD(...) ((void) __android_log_print(ANDROID_LOG_DEBUG, INB_LOG_TAG, __VA_ARGS__))

#else

#define INB_LOG_ENABLED 0

#define INB_LOGE(...) ((void) 0)
#define INB_LOGW(...) ((void) 0)
#define INB_LOGI(...) ((void) 0)
#define INB_LOGD(...) ((void) 0)

#endif
