#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <vector>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/detail/async_operation.hpp"
#include "coro/fd.hpp"
#include "coro/poll.hpp"

namespace coro::detail
{

using event_t = struct ::epoll_event;

class timer_handle;

class io_notifier_epoll
{
    static const constexpr std::size_t m_max_events = 16;
    static constexpr const int         m_wake_object{0};

    struct registration
    {
        async_operation* reader{nullptr};
        async_operation* writer{nullptr};
        void*            tag{nullptr};
        int              error{0};
    };

    fd_t                      m_fd;
    fd_t                      m_wake_fd{-1};
    std::mutex                m_mutex;
    std::vector<registration> m_registrations;
    std::vector<fd_t>         m_failed_fds;

    friend class detail::timer_handle;

public:
    /// Tag reported by next_events() after consuming a wake signal.
    static constexpr const void* wake_tag = &m_wake_object;

    io_notifier_epoll();

    io_notifier_epoll(const io_notifier_epoll&)                    = delete;
    io_notifier_epoll(io_notifier_epoll&&)                         = delete;
    auto operator=(const io_notifier_epoll&) -> io_notifier_epoll& = delete;
    auto operator=(io_notifier_epoll&&) -> io_notifier_epoll&      = delete;

    ~io_notifier_epoll();

    /// Wake a concurrent or the next next_events() call from any thread; multiple calls may coalesce.
    auto wake() -> void;

    auto watch_timer(const timer_handle& timer, std::chrono::nanoseconds duration) -> bool;

    auto watch(fd_t fd, poll_op op, void* data, bool keep = false) -> bool;

    static auto try_now(async_operation& op) -> bool;
    auto        submit(async_operation& op) -> bool;
    auto        complete_ready(async_operation& op, poll_status status) -> bool;
    /// Readiness backends release the operation immediately, so this always returns true.
    auto cancel(async_operation& op) -> bool;

    auto unwatch_timer(const timer_handle& timer) -> bool;

    auto next_events(
        std::vector<std::pair<async_operation*, poll_status>>& ready_events, std::chrono::milliseconds timeout) -> void;
};

} // namespace coro::detail
