#pragma once

#include "result.hpp"

#include <memory>

namespace INB {

    /** 通过原生入口地址安装和移除函数替换的后端接口。 */
    class HookBackend {
    public:
        virtual ~HookBackend() = default;

        /**
         * 将目标入口替换为调用方提供的函数。
         *
         * @param target 要替换的原生函数入口，不能为空，所属模块须在 Hook 生命周期内保持加载。
         * @param replacement 替换函数入口，不能为空，并须在 Hook 生命周期内保持可执行。
         * @return 原函数跳板地址，供 Hook 存续期间转发调用；安装失败时返回 backend 提供的错误。
         */
        virtual Result<void*> install(void* target, void* replacement) = 0;

        /**
         * 撤销此前对目标入口的替换。
         *
         * @param target 与 install() 使用的非空目标入口相同。
         * @return 撤销结果；失败时返回 backend 提供的错误。
         */
        virtual Result<void> remove(void* target) = 0;
    };

    namespace detail {
        struct HookState;
        /// 移除 HookState 对应的 Hook；状态为空时直接成功。
        Result<void> remove_hook(const std::shared_ptr<HookState>& state);
    }

} /// namespace INB
