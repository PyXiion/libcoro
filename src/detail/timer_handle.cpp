#include "coro/detail/timer_handle.hpp"

#include "coro/io_notifier.hpp"
#include "coro/platform.hpp"

namespace coro::detail
{

#if defined(CORO_PLATFORM_BSD)

static auto kqueue_current_timer_fd = std::atomic<fd_t>{0};

timer_handle::timer_handle(const void* timer_handle_ptr, io_notifier& notifier)
    : m_fd{kqueue_current_timer_fd++},
      m_timer_handle_ptr(timer_handle_ptr)
{
    (void)notifier;
}

#elif defined(CORO_PLATFORM_LINUX)

timer_handle::timer_handle(const void* timer_handle_ptr, io_notifier& notifier)
    : m_fd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      m_timer_handle_ptr(timer_handle_ptr)
{
    notifier.watch(m_fd, poll_op::read, const_cast<void*>(m_timer_handle_ptr), true);
}

#endif

} // namespace coro::detail
