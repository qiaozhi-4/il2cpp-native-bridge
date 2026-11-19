#include "hooks/abi_profile.hpp"

#include "facade/facade_internal.hpp"

#include <charconv>
#include <cstring>
#include <string_view>

namespace INB::detail {
    namespace {

        /// ECMA-335 MethodAttributes.Static；与 IL2CPP 布局无关。
        constexpr std::uint32_t kMethodAttributeStatic = 0x0010;

        struct UnityVersion {
            int major{};
            int minor{};
        };

        /// 读托管类型名；type_get_name 的返回值由 runtime 分配，读完立即释放。
        std::string type_name(const Api& api, const Il2CppType* type) {
            if (type == nullptr || api.type_get_name == nullptr) return {};
            char* raw = api.type_get_name(type);
            if (raw == nullptr) return {};
            std::string name{raw};
            if (api.free_memory != nullptr) api.free_memory(raw);
            return name;
        }

        /**
         * 在全部程序集里找 `UnityEngine.Application.get_unityVersion`。
         *
         * 不假设它属于哪个模块：Unity 把 Application 放在 UnityEngine.CoreModule，
         * 但那是实现细节，遍历程序集比硬编码模块名更稳。
         */
        const Il2CppMethodInfo* find_version_getter(const ReadyView& view) {
            const auto& api = view.api;
            std::size_t count = 0;
            const auto** assemblies = api.domain_get_assemblies(view.domain, &count);
            if (assemblies == nullptr) return nullptr;

            for (std::size_t index = 0; index < count; ++index) {
                const auto* image = api.assembly_get_image(assemblies[index]);
                if (image == nullptr) continue;
                Il2CppClass* klass = api.class_from_name(image, "UnityEngine", "Application");
                if (klass == nullptr) continue;

                void* iterator = nullptr;
                while (const auto* method = api.class_get_methods(klass, &iterator)) {
                    const char* name = api.method_get_name(method);
                    if (name == nullptr || std::strcmp(name, "get_unityVersion") != 0) continue;
                    /// 只接受已校验的「static、0 参数、返回 String」形态，其余候选直接跳过。
                    if (api.method_get_param_count(method) != 0) continue;
                    std::uint32_t implementation_flags = 0;
                    if ((api.method_get_flags(method, &implementation_flags) & kMethodAttributeStatic) == 0) continue;
                    if (type_name(api, api.method_get_return_type(method)) != "System.String") continue;
                    return method;
                }
            }
            return nullptr;
        }

        /// 解析 `6000.3.10f1` 这类版本串的前两段；其余部分不参与 family 判定。
        bool parse_version(std::string_view text, UnityVersion& out) {
            const auto first = text.find('.');
            if (first == std::string_view::npos) return false;
            const auto second = text.find('.', first + 1);
            if (second == std::string_view::npos) return false;

            const auto parse = [](std::string_view digits, int& value) {
                if (digits.empty()) return false;
                const auto* begin = digits.data();
                const auto* end = begin + digits.size();
                const auto parsed = std::from_chars(begin, end, value);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            return parse(text.substr(0, first), out.major) &&
                   parse(text.substr(first + 1, second - first - 1), out.minor);
        }

        std::string_view family_of(UnityVersion version) {
            if (version.major >= 2019 || (version.major == 2018 && version.minor >= 3)) return "modern-static";
            if (version.major == 2018 || version.major == 2017 || (version.major == 5 && version.minor >= 3)) {
                return "legacy-static-slot";
            }
            return {};
        }

        /**
         * 已在真机上跑通替换的 Unity 版本（major.minor）。
         *
         * 只有列在这里的版本才是 approved：参数槽、原生入口形态与 Gum 屏障都在该版本的设备
         * 运行中验过。2021.3 / 2022.3 取自目标应用（明日方舟 2021.3.39f1、无冬夜战 2022.3.62f2）。
         * 同一 family 的其他版本仍是 candidate —— 形态相同不等于已在该版本上验过。
         */
        constexpr struct {
            int major;
            int minor;
        } kVerifiedVersions[] = {{2021, 3}, {2022, 3}, {6000, 3}};

        bool is_verified_version(UnityVersion version) {
            for (const auto& entry : kVerifiedVersions) {
                if (entry.major == version.major && entry.minor == version.minor) return true;
            }
            return false;
        }

        /// 按 family 与版本填形态与证据等级；证据缺失的 family 只留名字并保持 rejected。
        AbiProfile profile_for(std::string_view family, UnityVersion version) {
            AbiProfile profile;
            profile.family = std::string{family};
            if (family == "modern-static") {
                /// 依据：Unity 6000.3.10f1 生成的 RuntimeInvoker 与 MethodInfo 布局，入口指针在 MethodInfo 首字段。
                profile.entry_offset = 0;
                profile.dummy_instance_static = false;
                profile.hidden_method_info = true;
                /// 运行时证据只覆盖已验证版本；同 family 的其他版本保持 candidate。
                profile.support = is_verified_version(version) ? ProfileSupport::approved : ProfileSupport::candidate;
            } else if (family == "legacy-static-slot") {
                /// 静态方法的隐含实例槽是 legacy family 的假设特征；没有生成源码可核对，
                /// 因此不填入口偏移与隐藏参数，且保持 rejected。
                profile.dummy_instance_static = true;
                profile.support = ProfileSupport::rejected;
            }
            return profile;
        }

        Result<AbiProfile> failure(ErrorCode code, std::string message) {
            return Result<AbiProfile>::failure(make_error(code, std::move(message)));
        }

    } /// namespace

    Result<AbiProfile> resolve_abi_profile(const ReadyView& view) {
        const auto& api = view.api;

        const auto* getter = find_version_getter(view);
        if (getter == nullptr) {
            return failure(ErrorCode::not_found,
                           "UnityEngine.Application.get_unityVersion is not available in this runtime");
        }
        if (api.runtime_invoke == nullptr) {
            return failure(ErrorCode::missing_api, "il2cpp_runtime_invoke is unavailable");
        }
        if (api.string_chars == nullptr || api.string_length == nullptr) {
            return failure(ErrorCode::missing_api, "the string access exports are unavailable");
        }

        Il2CppObject* exception = nullptr;
        auto* result = api.runtime_invoke(getter, nullptr, nullptr, &exception);
        /// 先看 exception：有异常时 result 无意义，不能当成 null 返回值。
        if (exception != nullptr) {
            return failure(ErrorCode::managed_exception, "reading the Unity version threw a managed exception");
        }
        if (result == nullptr) {
            return failure(ErrorCode::invocation_failed, "the Unity version getter returned no string");
        }

        auto* string = reinterpret_cast<Il2CppString*>(result);
        const auto* units = api.string_chars(string);
        const std::int32_t length = api.string_length(string);
        if (units == nullptr || length <= 0) {
            return failure(ErrorCode::invocation_failed, "the Unity version string could not be read");
        }

        /// 版本串是 ASCII；非 ASCII 单元转成占位符，让解析按未知版本处理。
        std::string text;
        text.reserve(static_cast<std::size_t>(length));
        for (std::int32_t index = 0; index < length; ++index) {
            const auto unit = units[index];
            text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
        }

        UnityVersion version{};
        if (!parse_version(text, version)) {
            return failure(ErrorCode::unsupported_runtime, "the Unity version '" + text + "' could not be parsed");
        }

        auto profile = profile_for(family_of(version), version);
        if (profile.family.empty()) {
            return failure(ErrorCode::unsupported_runtime, "no ABI profile covers Unity " + text);
        }
        profile.unity_version = std::move(text);
        return Result<AbiProfile>::success(std::move(profile));
    }

} /// namespace INB::detail
