#include "coro/detail/io_notifier_iocp.hpp"

#include "coro/detail/timer_handle.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <mswsock.h>
#include <windows.h>
#include <winternl.h>

namespace coro::detail
{

namespace
{
std::atomic<std::uint64_t> next_notifier_id{1};
} // namespace

io_notifier_iocp::io_notifier_iocp()
    : m_port{::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)},
      m_id{next_notifier_id.fetch_add(1, std::memory_order::relaxed)}
{
    if (m_port == nullptr)
    {
        throw std::runtime_error{"Failed to create I/O completion port"};
    }
}

io_notifier_iocp::~io_notifier_iocp()
{ ::CloseHandle(m_port); }

auto io_notifier_iocp::wake() -> void
{
    if (!m_wake_pending.exchange(true, std::memory_order::acq_rel))
    {
        if (!::PostQueuedCompletionStatus(m_port, 0, wake_key, nullptr))
        {
            auto error = ::GetLastError();
            m_wake_pending.store(false, std::memory_order::release);
            std::cerr << "::PostQueuedCompletionStatus(m_port) error[" << error << "] port=[" << m_port << "]"
                      << std::endl;
        }
    }
}

auto io_notifier_iocp::watch_timer(const timer_handle& timer, std::chrono::nanoseconds duration) -> bool
{
    auto deadline =
        (std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration))
            .time_since_epoch()
            .count();
    m_timer_tag.store(timer.get_inner(), std::memory_order::release);
    auto previous = m_deadline.exchange(deadline, std::memory_order::acq_rel);
    if (deadline < previous)
    {
        wake();
    }
    return true;
}

auto io_notifier_iocp::unwatch_timer(const timer_handle&) -> bool
{
    m_deadline.store(disarmed, std::memory_order::release);
    return true;
}

auto io_notifier_iocp::try_now(async_operation& op) -> bool
{ return perform_win32(op); }

auto io_notifier_iocp::associate(async_operation& op) -> int
{
    assert(op.m_iocp_binding != nullptr);
    // Submissions through one scheduler are serialized by its publication mutex, so only a foreign scheduler can
    // observe a binding whose association is still in progress; it fails like any other foreign submission.
    std::uint64_t owner{0};
    if (!op.m_iocp_binding->compare_exchange_strong(owner, m_id, std::memory_order::acq_rel))
    {
        return owner == m_id ? 0 : WSAEINVAL;
    }
    // Some Windows SDKs omit FileIoCompletionNotificationInformation (41) and its structure.
    struct file_io_completion_notification_information
    {
        ULONG Flags;
    };
    // Cast through void(*)() so compilers accept the FARPROC conversion without -Wcast-function-type.
    static const auto query_information = reinterpret_cast<decltype(&::NtQueryInformationFile)>(
        reinterpret_cast<void (*)()>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
    auto                                        handle = reinterpret_cast<HANDLE>(op.m_fd);
    IO_STATUS_BLOCK                             iosb{};
    file_io_completion_notification_information info{};
    // If the query is unavailable or fails, assume the mode is unset.
    if (query_information != nullptr &&
        query_information(handle, &iosb, &info, sizeof(info), static_cast<FILE_INFORMATION_CLASS>(41)) >= 0 &&
        (info.Flags & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0)
    {
        // Synchronous success must post a packet so the scheduler can retire the OVERLAPPED.
        op.m_iocp_binding->store(0, std::memory_order::release);
        return WSAEINVAL;
    }
    if (::CreateIoCompletionPort(handle, m_port, io_key, 0) == nullptr)
    {
        auto error = ::GetLastError();
        op.m_iocp_binding->store(0, std::memory_order::release);
        // ERROR_INVALID_PARAMETER: the handle was associated outside this socket object (e.g. a wrapped native
        // handle); its completions may go to another port, so refuse instead of waiting forever.
        return error == ERROR_INVALID_PARAMETER ? WSAEINVAL : static_cast<int>(error);
    }
    return 0;
}

auto io_notifier_iocp::submit(async_operation& op) -> bool
{
    if (op.m_kind == async_op_kind::timer)
    {
        op.m_submit_error = WSAEOPNOTSUPP;
        return false;
    }

    if (auto error = associate(op); error != 0)
    {
        op.m_submit_error = error;
        return false;
    }

    op.m_overlapped      = {};
    op.m_overlapped.m_op = &op;
    op.m_flags           = 0;
    op.m_iocp_truncated  = false;
    auto   fd            = static_cast<SOCKET>(op.m_fd);
    WSABUF buffer{
        static_cast<ULONG>(std::min(op.m_length, static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))),
        static_cast<char*>(op.m_buffer)};
    int result{SOCKET_ERROR};
    switch (op.m_kind)
    {
        case async_op_kind::read:
            result = ::WSARecv(fd, &buffer, 1, nullptr, &op.m_flags, &op.m_overlapped, nullptr);
            break;
        case async_op_kind::write:
            result = ::WSASend(fd, &buffer, 1, nullptr, 0, &op.m_overlapped, nullptr);
            break;
        case async_op_kind::recv_from:
        {
            auto [address, length] = op.m_address->native_mutable_data();
            *length                = sizeof(sockaddr_storage);
            result = ::WSARecvFrom(fd, &buffer, 1, nullptr, &op.m_flags, address, length, &op.m_overlapped, nullptr);
            break;
        }
        case async_op_kind::send_to:
        {
            auto [address, length] = op.m_address->data();
            result = ::WSASendTo(fd, &buffer, 1, nullptr, 0, address, length, &op.m_overlapped, nullptr);
            break;
        }
        case async_op_kind::accept:
        {
            static std::atomic<LPFN_ACCEPTEX> cached_accept_ex{nullptr};
            auto                              accept_ex = cached_accept_ex.load(std::memory_order::acquire);
            if (accept_ex == nullptr)
            {
                GUID  id = WSAID_ACCEPTEX;
                DWORD received{0};
                if (::WSAIoctl(
                        fd,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &id,
                        sizeof(id),
                        &accept_ex,
                        sizeof(accept_ex),
                        &received,
                        nullptr,
                        nullptr) == SOCKET_ERROR)
                {
                    op.m_submit_error = ::WSAGetLastError();
                    return false;
                }
                cached_accept_ex.store(accept_ex, std::memory_order::release);
            }

            sockaddr_storage listener_address{};
            int              listener_length{sizeof(listener_address)};
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&listener_address), &listener_length) == SOCKET_ERROR)
            {
                op.m_submit_error = ::WSAGetLastError();
                return false;
            }
            auto accepted = ::WSASocketW(
                listener_address.ss_family,
                SOCK_STREAM,
                IPPROTO_TCP,
                nullptr,
                0,
                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
            if (accepted == INVALID_SOCKET)
            {
                op.m_submit_error = ::WSAGetLastError();
                return false;
            }
            op.m_accepted = static_cast<net::native_handle_t>(accepted);
            static_assert(2 * (sizeof(sockaddr_in6) + 16) <= sizeof(sockaddr_storage));
            DWORD received{0};
            result = accept_ex(
                         fd,
                         accepted,
                         op.m_address->native_mutable_data().first,
                         0,
                         sizeof(sockaddr_in6) + 16,
                         sizeof(sockaddr_in6) + 16,
                         &received,
                         &op.m_overlapped)
                         ? 0
                         : SOCKET_ERROR;
            break;
        }
        case async_op_kind::connect:
        {
            static std::atomic<LPFN_CONNECTEX> cached_connect_ex{nullptr};
            auto                               connect_ex = cached_connect_ex.load(std::memory_order::acquire);
            if (connect_ex == nullptr)
            {
                GUID  id = WSAID_CONNECTEX;
                DWORD received{0};
                if (::WSAIoctl(
                        fd,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &id,
                        sizeof(id),
                        &connect_ex,
                        sizeof(connect_ex),
                        &received,
                        nullptr,
                        nullptr) == SOCKET_ERROR)
                {
                    op.m_submit_error = ::WSAGetLastError();
                    return false;
                }
                cached_connect_ex.store(connect_ex, std::memory_order::release);
            }

            auto [address, length] = op.m_address->data();
            sockaddr_storage local_address{};
            local_address.ss_family = address->sa_family;
            int local_length        = address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&local_address), local_length) == SOCKET_ERROR)
            {
                auto error = ::WSAGetLastError();
                if (error != WSAEINVAL)
                {
                    op.m_submit_error = error;
                    return false;
                }
            }
            result = connect_ex(fd, address, length, nullptr, 0, nullptr, &op.m_overlapped) ? 0 : SOCKET_ERROR;
            break;
        }
        case async_op_kind::timer:
            break;
    }

    if (result == SOCKET_ERROR)
    {
        auto error = ::WSAGetLastError();
        if (op.m_kind == async_op_kind::recv_from && error == WSAEMSGSIZE)
        {
            // Native Winsock documents no packet for an immediate error; Wine queues this warning before returning.
            // A FIFO marker retires both cases without releasing the operation ahead of Wine's native packet.
            op.m_iocp_truncated = true;
            op.m_registered     = true;
            if (!::PostQueuedCompletionStatus(m_port, 0, truncate_key, &op.m_overlapped))
            {
                auto post_error = ::GetLastError();
                std::cerr << "::PostQueuedCompletionStatus(m_port, truncate_key) error[" << post_error << "] port=["
                          << m_port << "]" << std::endl;
                std::abort();
            }
            return true;
        }
        if (error != WSA_IO_PENDING)
        {
            op.m_submit_error = error;
            if (op.m_kind == async_op_kind::accept)
            {
                ::closesocket(static_cast<SOCKET>(op.m_accepted));
                op.m_accepted = net::invalid_native_handle;
            }
            return false;
        }
    }
    // Association rejects skip-on-success mode, so synchronous success also queues a packet;
    // the OS owns the OVERLAPPED until it is dequeued.
    op.m_registered = true;
    return true;
}

auto io_notifier_iocp::complete_ready(async_operation& op, poll_status status) -> bool
{
    if (op.m_iocp_truncated)
    {
        // Only complete on the marker; an earlier native packet may have arrived in another event batch.
        if (status != poll_status::write)
        {
            return false;
        }
        op.m_iocp_truncated = false;
        op.m_registered     = false;
        op.m_bytes          = std::min(op.m_length, static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));
        op.m_status         = {};
        return true;
    }
    op.m_registered = false;
    DWORD bytes{0};
    DWORD flags{0};
    auto  fd    = static_cast<SOCKET>(op.m_fd);
    auto  ok    = ::WSAGetOverlappedResult(fd, &op.m_overlapped, &bytes, FALSE, &flags);
    auto  error = ok ? 0 : ::WSAGetLastError();
    if (error == WSA_OPERATION_ABORTED && op.m_abort_requested)
    {
        // Keep the scheduler's timeout or cancellation status.
    }
    else if (op.m_kind == async_op_kind::recv_from && error == WSAEMSGSIZE)
    {
        op.m_bytes  = std::min(op.m_length, static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));
        op.m_status = {};
    }
    else if (error != 0)
    {
        op.m_status = net::make_io_status_from_native(error);
    }
    else
    {
        op.m_status = {};
        switch (op.m_kind)
        {
            case async_op_kind::read:
                op.m_bytes = bytes;
                if (bytes == 0 && op.m_length > 0)
                {
                    op.m_status = {net::io_status::kind::closed};
                }
                break;
            case async_op_kind::write:
            case async_op_kind::recv_from:
            case async_op_kind::send_to:
                op.m_bytes = bytes;
                break;
            case async_op_kind::accept:
            {
                auto   accepted = static_cast<SOCKET>(op.m_accepted);
                u_long non_blocking{1};
                if (::setsockopt(
                        accepted,
                        SOL_SOCKET,
                        SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<const char*>(&fd),
                        sizeof(fd)) == SOCKET_ERROR)
                {
                    op.m_status = net::make_io_status_from_native(::WSAGetLastError());
                }
                else if (::ioctlsocket(accepted, FIONBIO, &non_blocking) == SOCKET_ERROR)
                {
                    op.m_status = net::make_io_status_from_native(::WSAGetLastError());
                }
                else
                {
                    auto [address, length] = op.m_address->native_mutable_data();
                    *length                = sizeof(sockaddr_storage);
                    if (::getpeername(accepted, address, length) == SOCKET_ERROR)
                    {
                        op.m_status = net::make_io_status_from_native(::WSAGetLastError());
                    }
                }
                break;
            }
            case async_op_kind::connect:
                if (::setsockopt(fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR)
                {
                    op.m_status = net::make_io_status_from_native(::WSAGetLastError());
                }
                break;
            case async_op_kind::timer:
                break;
        }
    }
    if (op.m_kind == async_op_kind::accept && !op.m_status.is_ok())
    {
        ::closesocket(static_cast<SOCKET>(op.m_accepted));
        op.m_accepted = net::invalid_native_handle;
    }
    return true;
}

auto io_notifier_iocp::cancel(async_operation& op) -> bool
{
    if (!op.m_registered)
    {
        return true;
    }
    if (op.m_iocp_truncated)
    {
        return false;
    }
    // ERROR_NOT_FOUND means that the completion is already on its way; it must still be drained.
    ::CancelIoEx(reinterpret_cast<HANDLE>(op.m_fd), &op.m_overlapped);
    return false;
}

auto io_notifier_iocp::next_events(
    std::vector<std::pair<async_operation*, poll_status>>& ready_events, std::chrono::milliseconds timeout) -> void
{
    constexpr auto max_wait = static_cast<std::chrono::milliseconds::rep>(INFINITE - 1);
    DWORD          wait     = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(std::min(timeout.count(), max_wait));
    auto           deadline = m_deadline.load(std::memory_order::acquire);
    if (deadline != disarmed)
    {
        auto remaining    = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{deadline}} -
                            std::chrono::steady_clock::now();
        auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        auto timer_wait   = static_cast<DWORD>(std::clamp(milliseconds, decltype(milliseconds){0}, max_wait));
        wait              = std::min(wait, timer_wait);
    }

    std::array<OVERLAPPED_ENTRY, m_max_events> entries;
    ULONG                                      removed{0};
    if (!::GetQueuedCompletionStatusEx(
            m_port, entries.data(), static_cast<ULONG>(entries.size()), &removed, wait, FALSE))
    {
        removed    = 0;
        auto error = ::GetLastError();
        if (error != WAIT_TIMEOUT)
        {
            std::cerr << "::GetQueuedCompletionStatusEx(m_port) error[" << error << "] port=[" << m_port << "]"
                      << std::endl;
            std::abort();
        }
    }
    for (ULONG i = 0; i < removed; ++i)
    {
        const auto& entry = entries[i];
        if (entry.lpCompletionKey == wake_key)
        {
            m_wake_pending.exchange(false, std::memory_order::acq_rel);
            ready_events.emplace_back(static_cast<async_operation*>(const_cast<void*>(wake_tag)), poll_status::read);
        }
        else
        {
            auto* op     = static_cast<overlapped_operation*>(entry.lpOverlapped)->m_op;
            auto  status = entry.lpCompletionKey == truncate_key ? poll_status::write : poll_status::read;
            ready_events.emplace_back(op, status);
        }
    }

    deadline = m_deadline.load(std::memory_order::acquire);
    if (deadline != disarmed && std::chrono::steady_clock::now().time_since_epoch().count() >= deadline)
    {
        auto* tag = m_timer_tag.load(std::memory_order::acquire);
        if (tag != nullptr)
        {
            ready_events.emplace_back(static_cast<async_operation*>(const_cast<void*>(tag)), poll_status::read);
        }
    }
}

} // namespace coro::detail
