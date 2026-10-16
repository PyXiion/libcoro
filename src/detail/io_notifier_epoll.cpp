#include "coro/detail/io_notifier_epoll.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <sys/eventfd.h>

#include "coro/detail/async_operation.hpp"
#include "coro/detail/timer_handle.hpp"
#include "coro/fd.hpp"
#include "coro/platform.hpp"
#include "coro/poll.hpp"

using namespace std::chrono_literals;

namespace coro::detail
{

static auto poll_op_to_events(poll_op op) -> uint32_t
{
    uint32_t events{0};
    if (poll_op_readable(op))
    {
        events |= EPOLLIN;
    }
    if (poll_op_writeable(op))
    {
        events |= EPOLLOUT;
    }
    return events;
}

io_notifier_epoll::io_notifier_epoll() : m_fd{::epoll_create1(EPOLL_CLOEXEC)}
{
    m_wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wake_fd == -1)
    {
        ::close(m_fd);
        throw std::runtime_error{"Failed to create eventfd"};
    }
    if (!watch(m_wake_fd, poll_op::read, const_cast<void*>(wake_tag), true))
    {
        ::close(m_wake_fd);
        ::close(m_fd);
        throw std::runtime_error{"Failed to register eventfd"};
    }
}

io_notifier_epoll::~io_notifier_epoll()
{
    if (m_wake_fd != -1)
    {
        ::close(m_wake_fd);
        m_wake_fd = -1;
    }
    if (m_fd != -1)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

auto io_notifier_epoll::wake() -> void
{
    uint64_t one = 1;
    while (::write(m_wake_fd, &one, sizeof(one)) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        // EAGAIN means the counter is saturated, the wake is already pending.
        if (errno != EAGAIN)
        {
            std::cerr << "::write(m_wake_fd) error[" << errno << "] " << ::strerror(errno) << " fd=[" << m_wake_fd
                      << "]" << std::endl;
        }
        break;
    }
}

auto io_notifier_epoll::watch_timer(const timer_handle& timer, std::chrono::nanoseconds duration) -> bool
{
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    duration -= seconds;
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

    // As a safeguard if both values end up as zero (or negative) then trigger the timeout
    // immediately as zero disarms timerfd according to the man pages and negative values
    // will result in an error return value.
    if (seconds <= 0s)
    {
        seconds = 0s;
        if (nanoseconds <= 0ns)
        {
            nanoseconds = 1ns;
        }
    }

    itimerspec ts{};
    ts.it_value.tv_sec  = seconds.count();
    ts.it_value.tv_nsec = nanoseconds.count();
    return ::timerfd_settime(timer.get_fd(), 0, &ts, nullptr) != -1;
}

auto io_notifier_epoll::watch(fd_t fd, poll_op op, void* data, bool keep) -> bool
{
    auto event_data     = event_t{};
    event_data.events   = poll_op_to_events(op) | EPOLLRDHUP | (keep ? EPOLLET : EPOLLONESHOT);
    event_data.data.ptr = data;
    return ::epoll_ctl(m_fd, EPOLL_CTL_ADD, fd, &event_data) != -1;
}

auto io_notifier_epoll::try_now(async_operation& op) -> bool
{
    return perform_posix(op);
}

auto io_notifier_epoll::submit(async_operation& op) -> bool
{
    if (!watch(op.m_fd, operation_poll_op(op), &op))
    {
        op.m_submit_error = errno;
        return false;
    }
    op.m_registered = true;
    return true;
}

auto io_notifier_epoll::complete_ready(async_operation& op, poll_status status) -> bool
{
    if (op.m_kind == async_op_kind::poll)
    {
        op.m_poll_status = status;
    }
    else if (!perform_posix(op))
    {
        auto event_data     = event_t{};
        event_data.events   = poll_op_to_events(operation_poll_op(op)) | EPOLLONESHOT | EPOLLRDHUP;
        event_data.data.ptr = &op;
        if (::epoll_ctl(m_fd, EPOLL_CTL_MOD, op.m_fd, &event_data) != -1)
        {
            return false;
        }
        op.m_status = net::make_io_status_from_native(errno);
    }
    cancel(op);
    return true;
}

auto io_notifier_epoll::cancel(async_operation& op) -> void
{
    if (op.m_registered)
    {
        unwatch(op.m_fd, operation_poll_op(op));
        op.m_registered = false;
    }
}

auto io_notifier_epoll::unwatch(fd_t fd, poll_op) -> bool
{
    return ::epoll_ctl(m_fd, EPOLL_CTL_DEL, fd, nullptr) != -1;
}

auto io_notifier_epoll::unwatch_timer(const timer_handle& timer) -> bool
{
    // Setting these values to zero disables the timer.
    itimerspec ts{};
    ts.it_value.tv_sec  = 0;
    ts.it_value.tv_nsec = 0;
    return ::timerfd_settime(timer.get_fd(), 0, &ts, nullptr) != -1;
}

auto io_notifier_epoll::next_events(
    std::vector<std::pair<async_operation*, poll_status>>& ready_events, std::chrono::milliseconds timeout) -> void
{
    auto ready_set = std::array<event_t, m_max_events>{};
    int  num_ready = ::epoll_wait(m_fd, ready_set.data(), ready_set.size(), timeout.count());
    for (int i = 0; i < num_ready; ++i)
    {
        auto* data = ready_set[i].data.ptr;
        if (data == wake_tag)
        {
            uint64_t value;
            ::read(m_wake_fd, &value, sizeof(value));
        }
        ready_events.emplace_back(static_cast<async_operation*>(data), event_to_poll_status(ready_set[i]));
    }
}

auto io_notifier_epoll::event_to_poll_status(const event_t& event) -> poll_status
{
    if (event.events & EPOLLIN)
    {
        return poll_status::read;
    }
    if (event.events & EPOLLOUT)
    {
        return poll_status::write;
    }
    else if (event.events & EPOLLERR)
    {
        return poll_status::error;
    }
    else if (event.events & EPOLLRDHUP || event.events & EPOLLHUP)
    {
        return poll_status::closed;
    }
    throw std::runtime_error{"invalid epoll state"};
}

} // namespace coro::detail
