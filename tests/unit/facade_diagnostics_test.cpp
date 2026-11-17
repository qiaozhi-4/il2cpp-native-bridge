#include "../frida_gum_stub/fake_frida_gum_control.hpp"
#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "facade/facade_internal.hpp"
#include "il2cpp_native_bridge/il2cpp.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

using namespace INB;

namespace {

using Outcome = test_support::ScopeOutcome;
using test_support::run_in_scope;

/// 把 stderr 的 fd 重定向到临时文件；用于验证「没有自定义 handler 时诊断仍然可见」。
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        path_ = "/tmp/il2cpp-bridge-stderr-" + std::to_string(::getpid()) + ".log";
        saved_ = ::dup(fileno(stderr));
        const int fd = ::open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd >= 0) {
            ::dup2(fd, fileno(stderr));
            ::close(fd);
        }
    }

    ~StderrCapture() {
        std::fflush(stderr);
        if (saved_ >= 0) {
            ::dup2(saved_, fileno(stderr));
            ::close(saved_);
        }
        ::unlink(path_.c_str());
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    [[nodiscard]] std::string read() const {
        std::fflush(stderr);
        std::ifstream input(path_);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

private:
    std::string path_;
    int saved_{-1};
};

struct DiagnosticsFixture {
    fake_il2cpp::Fixture fixture;
    Il2CppClass* counter{};

    DiagnosticsFixture() {
        fixture.add_corlib_types();
        counter = fixture.add_class("AssemblyA", "Probe", "Counter");
        fixture.add_method(counter, "Take", "System.Int32", {"System.Int32"}, true,
                           [this](Il2CppObject*, void** args, Il2CppObject**) -> Il2CppObject* {
                               return fixture.box(args[0], sizeof(int));
                           });
        fixture.add_method(counter, "Explode", "System.Int32", {}, true,
                           [this](Il2CppObject*, void**, Il2CppObject** exception) -> Il2CppObject* {
                               if (exception != nullptr) *exception = fixture.make_object(nullptr);
                               return nullptr;
                           });
    }
};

template <typename Callback>
Outcome run_method(const char* name, std::size_t parameter_count, Callback&& callback) {
    return run_in_scope([&](Outcome& outcome) {
        auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                              .method(name, parameter_count);
        callback(method, outcome);
    });
}

/// 让失败从 perform 的 action 逃逸：由库转成 Result 并输出默认诊断。
ErrorCode submit_failing(const std::function<void()>& body) {
    ErrorCode code = ErrorCode::internal_error;
    std::mutex mutex;
    std::condition_variable signal;
    bool done = false;

    const auto submitted = Il2Cpp::perform([&] { body(); }, [&](Result<void> result) {
        if (!result) code = result.error().code;
        std::lock_guard lock(mutex);
        done = true;
        signal.notify_all();
    },
    {});
    if (!submitted) return ErrorCode::internal_error;

    std::unique_lock lock(mutex);
    if (!signal.wait_for(lock, std::chrono::seconds(5), [&] { return done; })) {
        return ErrorCode::internal_error;
    }
    return code;
}

std::int32_t throwing_callback(ReplacementCall<std::int32_t, std::int32_t>&, std::int32_t) {
    throw std::runtime_error("callback exploded");
}

__attribute__((noinline)) std::int32_t explode_entry(std::int32_t value) { return value + 1; }

} /// namespace

void run_facade_diagnostics_tests() {
    /// 阶段与 path 区分：lookup / bind / invoke 各报自己的阶段。
    {
        test_support::DiagnosticCapture diagnostics;
        DiagnosticsFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());

        const auto lookup_code = submit_failing([] { (void) Il2Cpp::domain().assembly("Missing"); });
        test_support::check(lookup_code == ErrorCode::not_found, "an unknown assembly must fail with not_found");
        test_support::check(diagnostics.contains("stage=lookup"),
                            "a lookup failure must be reported at the lookup stage");
        test_support::check(diagnostics.contains("path=Missing"),
                            "the diagnostic path must carry the requested name only");

        const auto bind_code = submit_failing([] {
            auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                  .method("Take", 1);
            (void) method.typed<std::int32_t>();
        });
        test_support::check(bind_code != ErrorCode::internal_error, "a wrong argument count must be refused");
        test_support::check(diagnostics.contains("stage=bind"),
                            "a signature failure must be reported at the bind stage");
        test_support::check(diagnostics.contains("Probe.Counter::Take"),
                            "the bind path must name the declaring class and method");

        const auto invoke_code = submit_failing([] {
            auto method = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                  .method("Explode", 0);
            (void) method.invoke<std::int32_t>();
        });
        test_support::check(invoke_code == ErrorCode::managed_exception,
                            "a managed exception must surface as managed_exception");
        test_support::check(diagnostics.contains("stage=invoke"),
                            "a managed exception must be reported at the invoke stage");

        bool leak = false;
        for (const auto& line : diagnostics.lines()) {
            if (line.find("0x") != std::string::npos) leak = true;
        }
        test_support::check(!leak, "diagnostics must not print raw pointers or object identities");
    }

    /// 恢复阶段：flush 未确认时报告 restore。
    {
        test_support::DiagnosticCapture diagnostics;
        DiagnosticsFixture context;
        auto* method = context.fixture.add_method(context.counter, "Add", "System.Int32", {"System.Int32"}, true);
        context.fixture.set_method_entry(method, reinterpret_cast<void*>(&explode_entry));
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());

        const auto installed = run_method("Add", 1, [](const Method& target, Outcome&) {
            target.implementation<std::int32_t, std::int32_t>(&throwing_callback);
        });
        test_support::check(installed.ok, "the replacement must install before the restore check");

        fake_frida_gum::set_in_flight(true);
        const auto restore_code = submit_failing([] {
            auto target = Il2Cpp::domain().assembly("AssemblyA").image().klass("Probe.Counter")
                                  .method("Add", 1);
            target.revert();
        });
        test_support::check(restore_code == ErrorCode::restore_pending,
                            "an unconfirmed flush must report restore_pending");
        test_support::check(diagnostics.contains("stage=restore"),
                            "a restore failure must be reported at the restore stage");

        fake_frida_gum::set_in_flight(false);
        const auto cleaned = run_method("Add", 1, [](const Method& target, Outcome&) { target.revert(); });
        test_support::check(cleaned.ok, "the replacement must be restorable once the barrier clears");
    }

    /// 回调异常必须节流：100 次抛异常不能刷出 100 行诊断。
    {
        test_support::DiagnosticCapture diagnostics;
        DiagnosticsFixture context;
        auto* method = context.fixture.add_method(context.counter, "Add", "System.Int32", {"System.Int32"}, true);
        context.fixture.set_method_entry(method, reinterpret_cast<void*>(&explode_entry));
        fake_frida_gum::reset();
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());

        const auto installed = run_method("Add", 1, [](const Method& target, Outcome&) {
            target.implementation<std::int32_t, std::int32_t>(&throwing_callback);
        });
        test_support::check(installed.ok, "the throwing replacement must install");

        for (int index = 0; index < 100; ++index) {
            (void) fake_frida_gum::invoke<std::int32_t, std::int32_t, const void*>(
                    reinterpret_cast<void*>(&explode_entry), index, nullptr);
        }

        std::size_t callback_lines = 0;
        for (const auto& line : diagnostics.lines()) {
            if (line.find("category=callback") != std::string::npos) ++callback_lines;
        }
        test_support::check(callback_lines > 0, "a throwing callback must be reported at least once");
        test_support::check(callback_lines <= 2,
                            "repeated callback failures must be throttled, not reported 100 times");

        const auto cleaned = run_method("Add", 1, [](const Method& target, Outcome&) { target.revert(); });
        test_support::check(cleaned.ok, "the throttled replacement must be restorable");
    }

    /// 没有自定义 handler 时，诊断仍然写到 stderr。
    {
        DiagnosticsFixture context;
        test_support::FacadeHarness harness(context.fixture.api(), context.fixture.domain());
        StderrCapture capture;

        const auto code = submit_failing([] { (void) Il2Cpp::domain().assembly("Missing"); });
        test_support::check(code == ErrorCode::not_found, "the lookup must fail to produce a diagnostic");

        const auto text = capture.read();
        test_support::check(text.find("[il2cpp-native-bridge]") != std::string::npos,
                            "the default handler must still emit a module-prefixed line");
        test_support::check(text.find("stage=lookup") != std::string::npos,
                            "the default line must carry the failing stage");
    }

    /// 旧接口不变：Error 默认值、Result 语义、BridgeError 消息。
    {
        const Error legacy;
        test_support::check(legacy.stage == ErrorStage::none && legacy.path.empty(),
                            "the legacy Error must keep empty defaults");

        const auto success = Result<void>::success();
        test_support::check(success.has_value(), "Result<void> success must stay ok");

        const auto failure = Result<void>::failure(make_error(ErrorCode::not_found, "missing"));
        test_support::check(!failure && failure.error().code == ErrorCode::not_found,
                            "Result<void> failure must keep the original error");

        const BridgeError error(make_error(ErrorCode::not_found, ErrorStage::lookup, "A/B", "missing"));
        test_support::check(std::string(error.what()).find("missing") != std::string::npos,
                            "BridgeError must keep a readable message");
    }
}
