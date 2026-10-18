#pragma once

#include "coro/detail/async_operation.hpp"
#include "coro/poll.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace coro::detail
{

class timer_handle;

/// A socket is bound to the first scheduler that waits on it: IOCP association is permanent. The socket records the
/// id of that notifier; operations submitted through any other notifier fail with WSAEINVAL instead of hanging.
/// The single event consumer retires immediate UDP truncation by a FIFO marker after any synchronous native packet.
/// Failure to post that marker or an unexpected completion-port wait failure is fatal: ownership is uncertain.
class io_notifier_iocp
{
    static constexpr std::size_t m_max_events = 16;
    static constexpr int         m_wake_object{0};
    static constexpr ULONG_PTR   io_key{0};
    static constexpr ULONG_PTR   wake_key{1};
    static constexpr ULONG_PTR   truncate_key{2};
    static constexpr auto        disarmed = std::numeric_limits<std::chrono::steady_clock::rep>::max();

    HANDLE m_port;
    /// Never reused, unlike port handle values, so a socket bound to a destroyed scheduler cannot match a new one.
    const std::uint64_t                         m_id;
    std::atomic<bool>                           m_wake_pending{false};
    std::atomic<std::chrono::steady_clock::rep> m_deadline{disarmed};
    std::atomic<const void*>                    m_timer_tag{nullptr};

    /// Associates the operation's socket with this port once; returns 0 or a native error code.
    auto associate(async_operation& op) -> int;

public:
    /// Tag reported by next_events() after consuming a wake signal.
    static constexpr const void* wake_tag = &m_wake_object;

    io_notifier_iocp();

    io_notifier_iocp(const io_notifier_iocp&)                    = delete;
    io_notifier_iocp(io_notifier_iocp&&)                         = delete;
    auto operator=(const io_notifier_iocp&) -> io_notifier_iocp& = delete;
    auto operator=(io_notifier_iocp&&) -> io_notifier_iocp&      = delete;

    ~io_notifier_iocp();

    /// Wake a concurrent or the next next_events() call from any thread; multiple calls may coalesce.
    auto wake() -> void;

    auto watch_timer(const timer_handle& timer, std::chrono::nanoseconds duration) -> bool;
    auto unwatch_timer(const timer_handle& timer) -> bool;

    static auto try_now(async_operation& op) -> bool;
    auto        submit(async_operation& op) -> bool;
    auto        complete_ready(async_operation& op, poll_status status) -> bool;
    /// An in-flight operation remains OS-owned until its completion packet is dequeued.
    auto cancel(async_operation& op) -> bool;

    auto next_events(
        std::vector<std::pair<async_operation*, poll_status>>& ready_events, std::chrono::milliseconds timeout) -> void;
};

} // namespace coro::detail
