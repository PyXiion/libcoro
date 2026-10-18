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
    if (fd < 0)
    {
        errno = EBADF;
        return false;
    }
    std::scoped_lock lock{m_mutex};
    if (static_cast<std::size_t>(fd) >= m_registrations.size())
    {
        m_registrations.resize(static_cast<std::size_t>(fd) + 1);
    }
    auto event_data    = event_t{};
    event_data.events  = poll_op_to_events(op) | (keep ? EPOLLET : EPOLLONESHOT);
    event_data.data.fd = fd;
    if (::epoll_ctl(m_fd, EPOLL_CTL_ADD, fd, &event_data) == -1)
    {
        return false;
    }
    m_registrations[fd].tag = data;
    return true;
}

auto io_notifier_epoll::try_now(async_operation& op) -> bool
{ return perform_posix(op); }

auto io_notifier_epoll::submit(async_operation& op) -> bool
{
    if (op.m_fd < 0)
    {
        op.m_submit_error = EBADF;
        return false;
    }
    std::scoped_lock lock{m_mutex};
    if (static_cast<std::size_t>(op.m_fd) >= m_registrations.size())
    {
        m_registrations.resize(static_cast<std::size_t>(op.m_fd) + 1);
    }
    auto& current = m_registrations[op.m_fd];
    auto  next    = current;
    auto& slot    = operation_poll_op(op) == poll_op::read ? next.reader : next.writer;
    if (slot != nullptr || next.tag != nullptr)
    {
        op.m_submit_error = EEXIST;
        return false;
    }
    if (next.error != 0)
    {
        op.m_submit_error = next.error;
        return false;
    }
    slot               = &op;
    auto event_data    = event_t{};
    event_data.events  = (next.reader != nullptr ? EPOLLIN | EPOLLRDHUP : 0) | (next.writer != nullptr ? EPOLLOUT : 0);
    event_data.data.fd = op.m_fd;
    auto action        = current.reader == nullptr && current.writer == nullptr ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (::epoll_ctl(m_fd, action, op.m_fd, &event_data) == -1)
    {
        op.m_submit_error = errno;
        return false;
    }
    current         = next;
    op.m_registered = true;
    return true;
}

auto io_notifier_epoll::complete_ready(async_operation& op, poll_status) -> bool
{
    int error;
    {
        std::scoped_lock lock{m_mutex};
        error = m_registrations[op.m_fd].error;
    }
    if (error != 0)
    {
        op.m_status = net::make_io_status_from_native(error);
    }
    else if (!perform_posix(op))
    {
        return false;
    }
    cancel(op);
    return true;
}

auto io_notifier_epoll::cancel(async_operation& op) -> bool
{
    std::scoped_lock lock{m_mutex};
    if (op.m_registered)
    {
        auto& registration = m_registrations[op.m_fd];
        auto& slot         = operation_poll_op(op) == poll_op::read ? registration.reader : registration.writer;
        slot               = nullptr;
        op.m_registered    = false;
        if (registration.reader == nullptr && registration.writer == nullptr)
        {
            if (::epoll_ctl(m_fd, EPOLL_CTL_DEL, op.m_fd, nullptr) == -1 && errno != ENOENT && errno != EBADF)
            {
                std::cerr << "::epoll_ctl(DEL) error[" << errno << "] " << ::strerror(errno) << std::endl;
            }
            registration.error = 0;
        }
        else
        {
            auto event_data    = event_t{};
            event_data.events  = (registration.reader != nullptr ? EPOLLIN | EPOLLRDHUP : 0) |
                                 (registration.writer != nullptr ? EPOLLOUT : 0);
            event_data.data.fd = op.m_fd;
            if (::epoll_ctl(m_fd, EPOLL_CTL_MOD, op.m_fd, &event_data) == -1)
            {
                // The surviving operation must complete even if its readiness watch was lost.
                registration.error = errno;
                m_failed_fds.emplace_back(op.m_fd);
                wake();
            }
        }
    }
    return true;
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
    auto             ready_set = std::array<event_t, m_max_events>{};
    int              num_ready = ::epoll_wait(m_fd, ready_set.data(), ready_set.size(), timeout.count());
    std::scoped_lock lock{m_mutex};
    for (auto fd : m_failed_fds)
    {
        auto& registration = m_registrations[fd];
        if (registration.error != 0)
        {
            if (registration.reader != nullptr)
            {
                ready_events.emplace_back(registration.reader, poll_status::error);
            }
            if (registration.writer != nullptr)
            {
                ready_events.emplace_back(registration.writer, poll_status::error);
            }
        }
    }
    m_failed_fds.clear();
    for (int i = 0; i < num_ready; ++i)
    {
        const auto& event        = ready_set[i];
        auto&       registration = m_registrations[event.data.fd];
        if (registration.tag != nullptr)
        {
            if (registration.tag == wake_tag)
            {
                uint64_t value;
                ::read(m_wake_fd, &value, sizeof(value));
            }
            ready_events.emplace_back(static_cast<async_operation*>(registration.tag), poll_status::read);
            continue;
        }
        if (registration.error != 0)
        {
            continue;
        }
        const auto error  = (event.events & EPOLLERR) != 0;
        const auto closed = (event.events & EPOLLHUP) != 0;
        if (registration.reader != nullptr && (event.events & (EPOLLIN | EPOLLRDHUP) || error || closed))
        {
            auto status = poll_status::read;
            if (error)
            {
                status = poll_status::error;
            }
            else if (closed || (event.events & EPOLLRDHUP))
            {
                status = poll_status::closed;
            }
            ready_events.emplace_back(registration.reader, status);
        }
        if (registration.writer != nullptr && (event.events & EPOLLOUT || error || closed))
        {
            auto status = poll_status::write;
            if (error)
            {
                status = poll_status::error;
            }
            else if (closed)
            {
                status = poll_status::closed;
            }
            ready_events.emplace_back(registration.writer, status);
        }
    }
}

} // namespace coro::detail
