#include "coro/detail/io_notifier_kqueue.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "coro/detail/poll_info.hpp"
#include "coro/detail/timer_handle.hpp"
#include "coro/platform.hpp"
#include "coro/poll.hpp"

using namespace std::chrono_literals;

namespace coro::detail
{

static auto poll_op_to_filter(poll_op op) -> int16_t
{
    return op == poll_op::read ? EVFILT_READ : EVFILT_WRITE;
}

io_notifier_kqueue::io_notifier_kqueue() : m_fd{::kqueue()}
{
    auto event_data = event_t{};
    EV_SET(&event_data, m_wake_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, const_cast<void*>(wake_tag));
    if (::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) == -1)
    {
        if (m_fd != -1)
        {
            ::close(m_fd);
        }
        throw std::runtime_error{"failed to register kqueue wake event"};
    }
}

io_notifier_kqueue::~io_notifier_kqueue()
{
    if (m_fd != -1)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

auto io_notifier_kqueue::watch_timer(const timer_handle& timer, std::chrono::nanoseconds duration) -> bool
{
    // Prevent negative durations for the timeout as they will result in an error. 0 will fire in the next instance
    // possible.
    if (duration < 0ns)
    {
        duration = 0ns;
    }

    auto event_data = event_t{};
    EV_SET(
        &event_data,
        timer.get_fd(),
        EVFILT_TIMER,
        EV_ADD | EV_CLEAR | EV_ONESHOT,
        NOTE_NSECONDS,
        duration.count(),
        const_cast<void*>(timer.get_inner()));

    return ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) != -1;
}

auto io_notifier_kqueue::watch(fd_t fd, poll_op op, void* data, bool keep) -> bool
{
    // A read-write operation needs one registration for each kqueue filter.
    if (op == poll_op::read_write)
    {
        return watch(fd, poll_op::read, data, keep) && watch(fd, poll_op::write, data, keep);
    }

    auto event_data = event_t{};
    auto mode       = EV_ADD | EV_CLEAR | EV_ENABLE;
    if (!keep)
    {
        mode |= EV_ONESHOT;
    }

    EV_SET(&event_data, fd, poll_op_to_filter(op), mode, 0, 0, data);
    return ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) != -1;
}

auto io_notifier_kqueue::watch(poll_info& pi) -> bool
{
    if (!watch(pi.m_fd, pi.m_op, static_cast<void*>(&pi), false))
    {
        return false;
    }

    if (pi.m_cancel_trigger.has_value())
    {
        watch(pi.m_cancel_trigger.value().native_handle(), poll_op::read, static_cast<void*>(&pi));
    }

    return true;
}

auto io_notifier_kqueue::unwatch(fd_t fd, poll_op op) -> bool
{
    // For read-write event, we need to de-register both event types separately to the kqueue
    if (op == coro::poll_op::read_write)
    {
        auto event_data = event_t{};

        EV_SET(&event_data, fd, poll_op_to_filter(coro::poll_op::read), EV_DELETE, 0, 0, nullptr);
        ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr);

        EV_SET(&event_data, fd, poll_op_to_filter(coro::poll_op::write), EV_DELETE, 0, 0, nullptr);
        ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr);

        return true;
    }
    else
    {
        auto event_data = event_t{};
        EV_SET(&event_data, fd, poll_op_to_filter(op), EV_DELETE, 0, 0, nullptr);
        return ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) != -1;
    }
}

auto io_notifier_kqueue::unwatch(poll_info& pi) -> bool
{
    return unwatch(pi.m_fd, pi.m_op);
}

auto io_notifier_kqueue::unwatch_timer(const timer_handle& timer) -> bool
{
    auto event_data = event_t{};
    EV_SET(&event_data, timer.get_fd(), EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    return ::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) != -1;
}

auto io_notifier_kqueue::wake() -> void
{
    auto event_data = event_t{};
    EV_SET(&event_data, m_wake_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, const_cast<void*>(wake_tag));
    if (::kevent(m_fd, &event_data, 1, nullptr, 0, nullptr) == -1)
    {
        std::cerr << "failed to trigger kqueue wake event\n";
    }
}

auto io_notifier_kqueue::next_events(
    std::vector<std::pair<poll_info*, poll_status>>& ready_events, std::chrono::milliseconds timeout) -> void
{
    auto       ready_set       = std::array<event_t, m_max_events>{};
    const auto timeout_as_secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto       timeout_spec    = ::timespec{
                 .tv_sec  = timeout_as_secs.count(),
                 .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - timeout_as_secs).count(),
    };
    const int num_ready = ::kevent(
        m_fd, nullptr, 0, ready_set.data(), std::min(ready_set.size(), ready_events.capacity()), &timeout_spec);
    for (int i = 0; i < num_ready; i++)
    {
        if (ready_set[i].filter == EVFILT_USER && ready_set[i].udata == wake_tag)
        {
            ready_events.emplace_back(static_cast<poll_info*>(const_cast<void*>(wake_tag)), poll_status::read);
            continue;
        }

        auto* pi = static_cast<poll_info*>(ready_set[i].udata);
        if (ready_set[i].filter == EVFILT_TIMER)
        {
            ready_events.emplace_back(pi, event_to_poll_status(ready_set[i]));
            continue;
        }

        auto keep_registered = !(ready_set[i].flags & EV_ONESHOT);

        // If the event issuing fd is the same as the fd of the cancellation trigger of the registered poll_info we
        // this operation was cancelled by the user.
        if (pi->m_cancel_trigger.has_value() &&
            ready_set[i].ident == static_cast<uintptr_t>(pi->m_cancel_trigger.value().native_handle()))
        {
            ready_events.emplace_back(pi, poll_status::cancelled);
            if (!keep_registered)
            {
                unwatch(*pi);
            }
        }
        else
        {
            ready_events.emplace_back(pi, io_notifier_kqueue::event_to_poll_status(ready_set[i]));
            if (pi->m_cancel_trigger.has_value() && !keep_registered)
            {
                unwatch(pi->m_cancel_trigger.value().native_handle(), poll_op::read);
            }
        }
    }
}

auto io_notifier_kqueue::event_to_poll_status(const event_t& event) -> poll_status
{
    if ((event.filter == EVFILT_READ || event.filter == EVFILT_WRITE || event.filter == EVFILT_TIMER) &&
        event.flags & EV_EOF)
    {
        return poll_status::closed;
    }
    else if (
        (event.filter == EVFILT_READ || event.filter == EVFILT_WRITE || event.filter == EVFILT_TIMER) &&
        event.flags & EV_ERROR)
    {
        return poll_status::error;
    }
    else if (event.filter == EVFILT_READ || event.filter == EVFILT_USER)
    {
        return poll_status::read;
    }
    else if (event.filter == EVFILT_WRITE)
    {
        return poll_status::write;
    }
    else if (event.filter == EVFILT_TIMER)
    {
        // Due timer is handled like a read event by the lib
        return poll_status::read;
    }

    throw std::runtime_error{"invalid kqueue state"};
}

} // namespace coro::detail
