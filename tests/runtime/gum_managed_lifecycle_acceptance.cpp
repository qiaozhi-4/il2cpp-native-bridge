// 固定 Gum 17.23.3 的原生受控验收：不依赖 Unity、游戏或新门面。
//
// 只验证新 provider 依赖的 Gum 行为：replacement_data 分派、回调内原实现转发（TLS bypass）、
// 安装/恢复窗口内的并发调用、回调内自恢复、flush 未确认时的重试、恢复后回收并重装。
// 输出可脱敏的 JSON gate 报告；不包含设备标识、路径或托管内容。
#include <frida-gum.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct Record {
    std::uint32_t magic;
    std::atomic<int> calls;
    std::atomic<int> data_ok;
    std::atomic<int> original_ok;
    std::atomic<int> reverted_inside;
    std::atomic<bool> release{true};
    gpointer original;
    GumInterceptor* interceptor;
    gpointer target;
};

constexpr std::uint32_t kMagic = 0x47415445;  // "GATE"

__attribute__((noinline)) int target_add(int value) { return value + 1; }

/// 通过 volatile 函数指针调用，避免编译器把调用点内联/常量折叠成未 Hook 的路径。
int (*volatile call_target)(int) = target_add;

int replacement_add(int value) {
    GumInvocationContext* context = gum_interceptor_get_current_invocation();
    Record* record = context == nullptr
            ? nullptr
            : static_cast<Record*>(gum_invocation_context_get_replacement_data(context));

    if (record == nullptr || record->magic != kMagic) return -1;
    record->calls.fetch_add(1, std::memory_order_relaxed);
    record->data_ok.fetch_add(1, std::memory_order_relaxed);

    if (record->original != nullptr) {
        auto original = reinterpret_cast<int (*)(int)>(record->original);
        if (original(value) == value + 1) record->original_ok.fetch_add(1, std::memory_order_relaxed);
    }

    /// 自恢复：只在第一次回调里触发，之后目标应恢复原实现。
    if (record->reverted_inside.load(std::memory_order_relaxed) == 0 &&
        record->reverted_inside.exchange(1, std::memory_order_relaxed) == 0) {
        gum_interceptor_revert(record->interceptor, record->target);
        /// 停在回调内，让调用方在「仍处于 trampoline 内」时观察 flush 的返回值。
        while (!record->release.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return value + 100;
}

GumReplaceOptions options_for(Record* record) {
    GumReplaceOptions options;
    std::memset(&options, 0, sizeof(options));
    options.replacement_data = record;
    return options;
}

bool install(Record* record) {
    record->magic = kMagic;
    record->calls.store(0);
    record->data_ok.store(0);
    record->original_ok.store(0);
    record->reverted_inside.store(0);
    record->release.store(true);
    record->original = nullptr;

    GumReplaceOptions options = options_for(record);
    gpointer original = nullptr;
    GumReplaceReturn result = gum_interceptor_replace(
            record->interceptor, record->target, reinterpret_cast<gpointer>(replacement_add), &original, &options);
    record->original = original;
    return result == GUM_REPLACE_OK && original != nullptr;
}

/// 恢复后等待 Gum 确认不再有线程处于 trampoline 内；返回尝试次数，0 表示未确认。
int revert_and_flush(Record* record, int max_attempts) {
    gum_interceptor_revert(record->interceptor, record->target);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (gum_interceptor_flush_function(record->interceptor, record->target)) return attempt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* output_path = argc > 1 ? argv[1] : nullptr;

    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();

    const int baseline = call_target(1);

    /// 1) replacement_data 分派 + 回调内原实现转发（TLS bypass）
    Record first;
    first.interceptor = interceptor;
    first.target = reinterpret_cast<gpointer>(target_add);
    const bool installed = install(&first);
    const int hooked = installed ? call_target(1) : 0;
    const bool data_ok = first.calls.load() > 0 && first.data_ok.load() == first.calls.load();
    const bool original_ok = first.calls.load() > 0 && first.original_ok.load() == first.calls.load();
    const int first_flush = revert_and_flush(&first, 200);
    const bool restored = call_target(1) == baseline;

    /// 2) 安装窗口：另一线程持续调用，安装与恢复期间不得出现无效结果
    std::atomic<bool> stop{false};
    std::atomic<int> window_calls{0};
    std::atomic<int> window_bad{0};
    std::thread hammer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const int value = call_target(1);
            window_calls.fetch_add(1, std::memory_order_relaxed);
            if (value != baseline && value != 101) window_bad.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Record window;
    window.interceptor = interceptor;
    window.target = reinterpret_cast<gpointer>(target_add);
    const bool window_installed = install(&window);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int window_flush = revert_and_flush(&window, 200);
    stop.store(true, std::memory_order_relaxed);
    hammer.join();
    const bool window_ok = window_installed && window_flush > 0 && window_bad.load() == 0;

    /// 3) 恢复后回收并重装
    Record second;
    second.interceptor = interceptor;
    second.target = reinterpret_cast<gpointer>(target_add);
    const bool reinstalled = install(&second);
    const int rehooked = reinstalled ? call_target(1) : 0;
    const int second_flush = revert_and_flush(&second, 200);
    const bool second_restored = call_target(1) == baseline;

    /// 4) 自恢复 + 释放屏障：回调内 revert 后仍停在 trampoline 内，此时 flush 必须为 FALSE
    Record self;
    self.interceptor = interceptor;
    self.target = reinterpret_cast<gpointer>(target_add);
    const bool self_installed = install(&self);
    self.release.store(false);
    std::atomic<int> self_call{0};
    std::thread caller([&] { self_call.store(call_target(1)); });
    while (self.reverted_inside.load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool flush_while_inflight = gum_interceptor_flush_function(interceptor, self.target);
    self.release.store(true);
    caller.join();
    const int self_flush = revert_and_flush(&self, 200);
    const bool self_restored = call_target(1) == baseline;

    gum_deinit_embedded();

    const bool barrier_ok = !flush_while_inflight && self_flush > 0;
    const bool ok = installed && hooked == 101 && data_ok && original_ok && window_ok && first_flush > 0 &&
            restored && reinstalled && rehooked == 101 && second_flush > 0 && second_restored && self_installed &&
            self_call.load() == 101 && barrier_ok && self_restored;

    std::string report;
    report += "{\n";
    report += "  \"schemaVersion\": 1,\n";
    report += "  \"gate\": \"gum-managed-lifecycle\",\n";
    report += std::string("  \"ok\": ") + (ok ? "true" : "false") + ",\n";
    report += "  \"checks\": {\n";
    report += std::string("    \"replacementData\": ") + (data_ok ? "true" : "false") + ",\n";
    report += std::string("    \"originalForwarding\": ") + (original_ok ? "true" : "false") + ",\n";
    report += std::string("    \"installWindow\": ") + (window_ok ? "true" : "false") + ",\n";
    report += std::string("    \"flushConfirmed\": ") + (first_flush > 0 ? "true" : "false") + ",\n";
    report += std::string("    \"restoredAfterRevert\": ") + (restored ? "true" : "false") + ",\n";
    report += std::string("    \"reinstalledAfterReclaim\": ") + (reinstalled && rehooked == 101 ? "true" : "false") + ",\n";
    report += std::string("    \"selfRevert\": ") + (self_restored ? "true" : "false") + ",\n";
    report += std::string("    \"flushBlocksWhileInFlight\": ") + (barrier_ok ? "true" : "false") + "\n";
    report += "  },\n";
    report += "  \"measurements\": {\n";
    report += "    \"baselineValue\": " + std::to_string(baseline) + ",\n";
    report += "    \"hookedValue\": " + std::to_string(hooked) + ",\n";
    report += "    \"callbackCount\": " + std::to_string(first.calls.load()) + ",\n";
    report += "    \"windowCalls\": " + std::to_string(window_calls.load()) + ",\n";
    report += "    \"windowInvalidValues\": " + std::to_string(window_bad.load()) + ",\n";
    report += "    \"flushAttempts\": " + std::to_string(first_flush) + "\n";
    report += "  }\n";
    report += "}\n";

    std::fputs(report.c_str(), stdout);
    if (output_path != nullptr) {
        if (FILE* file = std::fopen(output_path, "wb")) {
            std::fwrite(report.data(), 1, report.size(), file);
            std::fclose(file);
        }
    }
    return ok ? 0 : 1;
}
