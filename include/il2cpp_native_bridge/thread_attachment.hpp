#pragma once

#include "result.hpp"

#include <memory>

namespace INB {

    class Runtime;
    namespace detail {
        struct RuntimeContext;
        struct ThreadLease;
    }

    /**
     * 管理当前线程的 IL2CPP 附加生命周期；对象销毁时只分离由 bridge 附加的线程。
     *
     * 此作用域不可复制，可移动转移所有权。
     */
    class ThreadAttachment {
    public:
        ThreadAttachment(ThreadAttachment&&) noexcept;
        ThreadAttachment& operator=(ThreadAttachment&&) noexcept;
        ThreadAttachment(const ThreadAttachment&) = delete;
        ThreadAttachment& operator=(const ThreadAttachment&) = delete;
        ~ThreadAttachment();

        /**
         * 尝试将调用线程附加到指定 runtime。
         *
         * @param runtime 当前进程中的 IL2CPP runtime。
         * @return 持有附加作用域的对象；runtime 未就绪或附加所需 API 不可用时返回错误。
         */
        static Result<ThreadAttachment> attach(const Runtime& runtime);

    private:
        explicit ThreadAttachment(std::unique_ptr<detail::ThreadLease> lease);
        std::unique_ptr<detail::ThreadLease> lease_;
    };

} /// namespace INB
