#pragma once

#include "coro/platform.hpp"

#if defined(CORO_PLATFORM_BSD)
    #include "coro/detail/io_notifier_kqueue.hpp"
#elif defined(CORO_PLATFORM_LINUX)
    #include "coro/detail/io_notifier_epoll.hpp"
#endif

namespace coro
{

#if defined(CORO_PLATFORM_BSD)
using io_notifier = detail::io_notifier_kqueue;
#elif defined(CORO_PLATFORM_LINUX)
using io_notifier = detail::io_notifier_epoll;
#endif

} // namespace coro
