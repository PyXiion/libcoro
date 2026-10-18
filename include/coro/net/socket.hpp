#pragma once

#include "coro/net/detail/socket_api.hpp"
#include "coro/net/socket_address.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <utility>

#include <iostream>

namespace coro
{
class scheduler;
} // namespace coro

namespace coro::net
{
/**
 * Owns a native socket handle; move-only.
 * On Windows a socket is bound to the first scheduler that waits on it (IOCP association is permanent):
 * waiting operations through any other scheduler fail with a native WSAEINVAL io_status.
 */
class socket
{
public:
    enum class type_t
    {
        /// udp datagram socket
        udp,
        /// tcp streaming socket
        tcp
    };

    enum class blocking_t
    {
        /// This socket should block on system calls.
        yes,
        /// This socket should not block on system calls.
        no
    };

    enum class shutdown_how
    {
        read,
        write,
        read_write
    };

    struct options
    {
        /// The type of socket.
        type_t type;
        /// If the socket should be blocking or non-blocking.
        blocking_t blocking;
    };

    static auto type_to_os(type_t type) -> int;

    socket() = default;
    /**
     * Adopts a native socket handle.
     * On Windows the handle must not have FILE_SKIP_COMPLETION_PORT_ON_SUCCESS enabled: such handles are refused
     * with a native WSAEINVAL io_status on first scheduler use. Enabling this mode after first scheduler use is
     * unsupported.
     */
    explicit socket(native_handle_t handle) : m_handle(handle) {}

    socket(const socket&) = delete;
    socket(socket&& other) noexcept : m_handle(std::exchange(other.m_handle, invalid_native_handle))
    {
#if defined(CORO_PLATFORM_WINDOWS)
        m_iocp_binding.store(other.m_iocp_binding.exchange(0, std::memory_order::relaxed), std::memory_order::relaxed);
#endif
    }
    auto operator=(const socket&) -> socket& = delete;
    auto operator=(socket&& other) noexcept -> socket&;

    ~socket() { close(); }

    /**
     * This function returns true if the socket's native handle is valid, however it does
     * not imply if the socket is still usable.
     * @return True if the socket has a valid native handle.
     */
    [[nodiscard]] auto is_ok() const -> bool { return m_handle != invalid_native_handle; }

    explicit operator bool() const { return is_ok(); }

    /**
     * @param block Sets the socket to the given blocking mode.
     */
    auto blocking(blocking_t block) -> bool;

    /**
     * @param how Shuts the socket down with the given operations.
     * @return Returns true if the sockets given operations were shutdown.
     */
    auto shutdown(shutdown_how how = shutdown_how::read_write) -> bool;

    /**
     * Closes the socket and sets this socket to an invalid state.
     */
    auto close() -> void;

    /**
     * @return The native handle (file descriptor) for this socket.
     */
    auto native_handle() const -> native_handle_t { return m_handle; }

private:
    friend class ::coro::scheduler;

    native_handle_t m_handle{invalid_native_handle};
#if defined(CORO_PLATFORM_WINDOWS)
    /// Id of the scheduler event notifier this socket is associated with, 0 if none yet.
    std::atomic<std::uint64_t> m_iocp_binding{0};
#endif
};

/**
 * Creates a socket with the given socket options, this typically is used for creating sockets to
 * use within client objects, e.g. tcp::client and udp::client.
 * @param opts See socket::options for more details.
 * TODO: docs
 */
auto make_socket(const socket::options& opts, domain_t) -> socket;

/**
 * Creates a socket that can accept connections or packets with the given socket options, address,
 * port and backlog.  This is used for creating sockets to use within server objects, e.g.
 * tcp::server and udp::server.
 * @param opts See socket::options for more details
 * @param address The ip address to bind to.  If the type of socket is tcp then it will also listen.
 * @param port The port to bind to.
 * @param backlog If the type of socket is tcp then the backlog of connections to allow.  Does nothing
 *                for udp types.
 * TODO: docs
 */
auto make_accept_socket(const socket::options& opts, const net::socket_address& endpoint, int32_t backlog) -> socket;

} // namespace coro::net
