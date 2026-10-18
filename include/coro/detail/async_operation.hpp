#pragma once

#include "coro/net/io_status.hpp"
#include "coro/net/socket_address.hpp"
#include "coro/platform.hpp"
#include "coro/poll.hpp"
#include "coro/time.hpp"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <map>
#include <optional>

namespace coro::detail
{

#if defined(CORO_PLATFORM_WINDOWS)
struct async_operation;

/// OVERLAPPED owned by the OS from a successful submit() until its completion packet is dequeued.
struct overlapped_operation : OVERLAPPED
{
    async_operation* m_op{nullptr};
};
#endif

enum class async_op_kind : std::uint8_t
{
    timer,
    read,
    write,
    accept,
    connect,
    recv_from,
    send_to
};

struct async_operation
{
    using timed_events = std::multimap<coro::time_point, async_operation*>;

    static constexpr std::uint8_t state_suspended     = 1;
    static constexpr std::uint8_t state_finished      = 2;
    static constexpr std::uint8_t state_cancel_queued = 4;
    static constexpr std::uint8_t state_completed     = 8;

    async_operation()                                          = default;
    async_operation(const async_operation&)                    = delete;
    async_operation(async_operation&&)                         = delete;
    auto operator=(const async_operation&) -> async_operation& = delete;
    auto operator=(async_operation&&) -> async_operation&      = delete;

    async_op_kind        m_kind{async_op_kind::timer};
    net::native_handle_t m_fd{net::invalid_native_handle};
    void*                m_buffer{nullptr};
    std::size_t          m_length{0};
    net::socket_address* m_address{nullptr};

    net::io_status       m_status{};
    std::size_t          m_bytes{0};
    net::native_handle_t m_accepted{net::invalid_native_handle};
    int                  m_submit_error{0};
    bool                 m_connect_started{false};
    bool                 m_registered{false};
    // Scheduler-owned: the abort status is stored in m_status; an OS completion reporting cancellation keeps it.
    bool m_abort_requested{false};

#if defined(CORO_PLATFORM_WINDOWS)
    overlapped_operation m_overlapped{};
    DWORD                m_flags{0};
    // Publication-mutex-owned: immediate UDP truncation retires at its marker after any native packet.
    bool m_iocp_truncated{false};
    // The socket's record of the notifier it is associated with; set for every socket operation.
    std::atomic<std::uint64_t>* m_iocp_binding{nullptr};
#endif

    std::atomic<std::uint8_t> m_state{0};
    // Accessed only under scheduler::m_timed_events_mutex.
    std::optional<timed_events::iterator> m_timer_pos{};
    std::coroutine_handle<>               m_awaiting_coroutine{};
    async_operation*                      m_next{nullptr};
};

#if defined(CORO_PLATFORM_WINDOWS)
auto perform_win32(async_operation& op) -> bool;
#else
auto perform_posix(async_operation& op) -> bool;
#endif

inline auto operation_poll_op(const async_operation& op) -> coro::poll_op
{
    return op.m_kind == async_op_kind::write || op.m_kind == async_op_kind::connect ||
                   op.m_kind == async_op_kind::send_to
               ? coro::poll_op::write
               : coro::poll_op::read;
}

} // namespace coro::detail
