#include "../runtime/fake_il2cpp.hpp"
#include "../test_support.hpp"
#include "il2cpp_native_bridge/bridge.hpp"
#include "runtime/runtime_internal.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace INB;

namespace {

constexpr int kThreadCount = 8;
constexpr int kIterations = 200;

} /// namespace

/// 并发查询同一类：所有线程必须看到一致结果，且不得崩溃。
/// 数据竞争的真正检测依赖 -DIL2CPP_NATIVE_BRIDGE_SANITIZE=thread，这里负责结果一致性。
void run_concurrency_tests() {
    fake_il2cpp::Fixture fixture;
    const auto runtime = detail::make_testing_runtime(fixture.api(), fixture.domain());

    auto reference = runtime.klass("UnityEngine.Time");
    test_support::check(reference.found(), "the reference class must resolve before the concurrent run");
    const std::string reference_assembly(reference.assembly_name());
    test_support::check(!reference_assembly.empty(), "the reference class must report an assembly");

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::atomic<int> assembly_mismatches{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int index = 0; index < kThreadCount; ++index) {
        workers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

            for (int iteration = 0; iteration < kIterations; ++iteration) {
                auto klass = runtime.klass("UnityEngine.Time");
                if (!klass.found()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (std::string(klass.assembly_name()) != reference_assembly) {
                    assembly_mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                if (!klass.method<void, float>("set_timeScale").found()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != kThreadCount) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    test_support::check(failures.load() == 0, "every concurrent class and method lookup must succeed");
    test_support::check(assembly_mismatches.load() == 0,
                        "concurrent lookups must report the same assembly name");
}
