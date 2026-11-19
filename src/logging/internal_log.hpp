#pragma once

/**
 * il2cpp-native-bridge 内部日志。
 *
 * 风格参考 BNM：用编译期开关控制。日志只写 logcat，不依赖宿主 Logger，也不进 ImGui 面板。
 *
 * 只有两档，由 CMake 是否定义 INB_ENABLE_LOGCAT 决定：
 * - 定义（Android Debug）：全量输出 E/W/I/D。
 * - 未定义（Release 等）：只输出错误，告警与轨迹全部编译期丢弃。
 *
 * INB_LOG_ENABLED 是同一开关的 C++ 常量形式，供 `if constexpr` 使用：宏只管「调不调用
 * __android_log_print」，调用点周围的配套代码（去重查表、字符串拼接、计时）要靠它一起丢弃，
 * 否则关掉轨迹后这些逻辑仍会照常执行。
 */
#if defined(__ANDROID__)
#include <android/log.h>
#define INB_HOST_LOG(level, ...) ((void) __android_log_print(level, INB_LOG_TAG, __VA_ARGS__))
#else
#include <cstdio>
#define INB_HOST_LOG(level, ...) ((void) std::fprintf(stderr, "[" INB_LOG_TAG "] " __VA_ARGS__))
#endif

#define INB_LOG_TAG "INB"

#if defined(INB_ENABLE_LOGCAT)

/// 轨迹可用（全量）。
#define INB_LOG_ENABLED 1

#define INB_LOGE(...) INB_HOST_LOG(ANDROID_LOG_ERROR, __VA_ARGS__)
#define INB_LOGW(...) INB_HOST_LOG(ANDROID_LOG_WARN, __VA_ARGS__)
#define INB_LOGI(...) INB_HOST_LOG(ANDROID_LOG_INFO, __VA_ARGS__)
#define INB_LOGD(...) INB_HOST_LOG(ANDROID_LOG_DEBUG, __VA_ARGS__)

#else

/// 只留错误。
#define INB_LOG_ENABLED 0

#define INB_LOGE(...) INB_HOST_LOG(ANDROID_LOG_ERROR, __VA_ARGS__)
#define INB_LOGW(...) ((void) 0)
#define INB_LOGI(...) ((void) 0)
#define INB_LOGD(...) ((void) 0)

#endif
