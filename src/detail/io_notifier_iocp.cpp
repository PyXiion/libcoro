#include "coro/detail/io_notifier_iocp.hpp"
#include "coro/detail/signal_win32.hpp"
#include "coro/detail/timer_handle.hpp"

#include <array>
#include <coro/detail/iocp_overlapped.hpp>

namespace coro::detail
{
io_notifier_iocp::io_notifier_iocp()
{
    DWORD concurrent_threads = 0; // TODO

    m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, concurrent_threads);
}

io_notifier_iocp::~io_notifier_iocp()
{
    CloseHandle(m_iocp);
}

auto io_notifier_iocp::watch_timer(detail::timer_handle& timer, std::chrono::nanoseconds duration) -> bool
{
    if (timer.m_iocp == nullptr)
    {
        timer.m_iocp = m_iocp;
    }
    else if (timer.m_iocp != m_iocp)
    {
        throw std::runtime_error("Timer is already associated with a different IOCP handle. Cannot reassign.");
    }

    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -duration.count() / 100; // time in 100ns intervals, negative for relative

    // `timer_handle` must remain alive until the timer fires.
    // This is guaranteed by `io_scheduler`, which owns the timer lifetime.
    //
    // We could allocate a separate `timer_context` on the heap to decouple ownership,
    // but safely freeing it is difficult without introducing overhead or leaks:
    // the timer can be cancelled, and we have no guaranteed way to retrieve the pointer.
    //
    // Therefore, we directly pass a pointer to `timer_handle` as the APC context.
    // This avoids allocations and should be safe (I hope) under our scheduler's lifetime guarantees.

    if (timer.m_wait_handle != nullptr)
    {
        unwatch_timer(timer);
    }

    BOOL ok = SetWaitableTimer(timer.get_native_handle(), &dueTime, 0, nullptr, nullptr, false);

    if (!ok)
        return false;

    ok = RegisterWaitForSingleObject(
        &timer.m_wait_handle,
        timer.get_native_handle(),
        [](PVOID timer_ptr, BOOLEAN)
        {
            const auto timer = static_cast<detail::timer_handle*>(timer_ptr);
            PostQueuedCompletionStatus(
                timer->get_iocp(), 0, static_cast<int>(completion_key::timer), reinterpret_cast<LPOVERLAPPED>(timer));
        },
        &timer,
        INFINITE,
        WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION);
    return ok;
}

auto io_notifier_iocp::unwatch_timer(detail::timer_handle& timer) -> bool
{
    if (timer.m_wait_handle == nullptr)
    {
        return false;
    }
    CancelWaitableTimer(timer.get_native_handle());
    UnregisterWaitEx(timer.m_wait_handle, INVALID_HANDLE_VALUE);
    timer.m_wait_handle = nullptr;
    return true;
}

auto io_notifier_iocp::watch(coro::signal& signal, void* data) -> bool
{
    signal.m_iocp = m_iocp;
    signal.m_data = data;
    return true;
}

/**
 * I think this cycle needs a little explanation.
 *
 *  == Completion keys ==
 *
 *  1. **Signals**
 *      IOCP is not like epoll or kqueue, it works only with file-related events.
 *      To emulate signals io_scheduler uses (previously a pipe, now abstracted into signals)
 *      I use an array that tracks all active signals and dispatches them on every call.
 *
 *      Because of this, we need a pointer to the IOCP handle inside `@ref coro::signal`.
 *
 *  2. **Sockets**
 *      It's nothing special. We just get the pointer to poll_info through `@ref coro::detail::overlapped_poll_info`.
 *      The overlapped structure is stored inside the coroutine, so as long as coroutine lives everything will be fine.
 *      But if the coroutine dies, it's UB. I see no point in using heap, since if we have no coroutine, what should
 *      we dispatch?
 *
 *      **Important**
 *      All sockets **must** have the following flags set using `SetFileCompletionNotificationModes`:
 *
 *      - `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`:
 *          Prevents IOCP from enqueuing completions if the operation completes synchronously.
 *          If disabled, IOCP might try to access an `OVERLAPPED` structure from a coroutine that has already died.
 *          This can cause undefined behavior if the coroutine is dead and its memory is invalid.
 *          If it's still alive - you got lucky.
 *
 *      - `FILE_SKIP_SET_EVENT_ON_HANDLE`:
 *          Prevents the system from setting a WinAPI event on the socket handle.
 *          We don’t use system events, so this is safe and gives a small performance boost.
 *
 *  3. Timers
 *       IOCP doesn’t support timers directly - Windows has no `timerfd` like Unix.
 *       We use waitable timers (see `timer_handle.cpp`) to emulate this.
 *       When the timer fires, it triggers `@ref onTimerFired`, which posts an event to the IOCP queue.
 *       Since it's our own event we don't have to pass a valid OVERLAPPED structure,
 *       we just pass a pointer to the timer data and then emplace it into `ready_events`.
 *
 */
auto io_notifier_iocp::next_events(
    std::vector<std::pair<detail::poll_info*, coro::poll_status>>& ready_events,
    const std::chrono::milliseconds                                timeout) -> void
{
    using namespace std::chrono;

    auto handle = [&](const DWORD bytes, const completion_key key, const LPOVERLAPPED ov)
    {
        switch (key)
        {
            case completion_key::signal_set:
            case completion_key::signal_unset:
                if (ov)
                    set_signal_active(ov, key == completion_key::signal_set);
                break;
            case completion_key::socket:
                if (ov)
                {
                    auto* info              = reinterpret_cast<overlapped_io_operation*>(ov);
                    info->bytes_transferred = bytes;
                    ready_events.emplace_back(&info->pi, coro::poll_status::event);
                }
                break;
            case completion_key::timer:
                if (ov)
                {
                    auto timer = reinterpret_cast<detail::timer_handle*>(ov);
                    ready_events.emplace_back(
                        static_cast<detail::poll_info*>(const_cast<void*>(timer->get_inner())),
                        coro::poll_status::event);

                    UnregisterWaitEx(timer->m_wait_handle, INVALID_HANDLE_VALUE);
                    timer->m_wait_handle = nullptr;
                    std::atomic_thread_fence(std::memory_order::release);
                }
                break;
            default:
                throw std::runtime_error("Unknown completion key");
        }
    };

    process_active_signals(ready_events);

    std::array<OVERLAPPED_ENTRY, max_events> entries{};
    ULONG                                    number_of_events{};
    const DWORD dword_timeout = (timeout <= 0ms) ? INFINITE : static_cast<DWORD>(timeout.count());

    if (const BOOL ok = GetQueuedCompletionStatusEx(
            m_iocp, entries.data(), entries.size(), &number_of_events, dword_timeout, FALSE);
        !ok)
    {
        const DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT)
        {
            // No events available
            return;
        }

        throw std::system_error(static_cast<int>(err), std::system_category(), "GetQueuedCompletionStatusEx failed.");
    }

    for (ULONG i = 0; i < number_of_events; ++i)
    {
        const auto& e     = entries[i];
        const auto  key   = static_cast<completion_key>(e.lpCompletionKey);
        const auto  ov    = e.lpOverlapped;
        const auto  bytes = e.dwNumberOfBytesTransferred;

        handle(bytes, key, ov);
    }
}

void io_notifier_iocp::set_signal_active(void* data, bool active)
{
    std::scoped_lock lk{m_active_signals_mutex};
    if (active)
    {
        m_active_signals.emplace_back(data);
    }
    else if (auto it = std::find(m_active_signals.begin(), m_active_signals.end(), data); it != m_active_signals.end())
    {
        // Fast erase
        std::swap(m_active_signals.back(), *it);
        m_active_signals.pop_back();
    }
}
void io_notifier_iocp::process_active_signals(
    std::vector<std::pair<detail::poll_info*, coro::poll_status>>& ready_events)
{
    for (void* data : m_active_signals)
    {
        // poll_status doesn't matter.
        ready_events.emplace_back(static_cast<poll_info*>(data), poll_status::event);
    }
}
} // namespace coro::detail