#pragma once

#include "coro/net/detail/socket_api.hpp"
#include "coro/net/socket_address.hpp"

#include <span>
#include <utility>

#include <iostream>

namespace coro::net
{
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
    explicit socket(native_handle_t handle) : m_handle(handle) {}

    socket(const socket& other);
    socket(socket&& other) noexcept : m_handle(std::exchange(other.m_handle, invalid_native_handle)) {}
    auto operator=(const socket& other) noexcept -> socket&;
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
    native_handle_t m_handle{invalid_native_handle};
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
