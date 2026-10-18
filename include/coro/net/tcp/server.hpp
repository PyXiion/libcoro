#pragma once

#include "coro/net/ip_address.hpp"
#include "coro/net/socket.hpp"
#include "coro/net/tcp/client.hpp"
#include "coro/task.hpp"

#include <stop_token>

namespace coro
{

class scheduler;
} // namespace coro

namespace coro::net::tcp
{

class server
{
public:
    struct options
    {
        /// The kernel backlog of connections to buffer.
        int32_t backlog{128};
    };

    explicit server(
        std::unique_ptr<coro::scheduler>& scheduler,
        const net::socket_address&        endpoint,
        options                           opts = options{

            .backlog = 128,
        });

    server(const server&) = delete;
    server(server&& other);
    auto operator=(const server&) -> server& = delete;
    auto operator=(server&& other) -> server&;
    ~server() = default;

    /**
     * Asynchronously waits for an incoming TCP connection and accepts it.
     *
     * @param timeout How long to wait for a new connection before timing out, zero waits indefinitely.
     * @return The newly connected tcp client connection on success or an io_status describing the failure.
     */
    auto accept(std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
        -> coro::task<expected<coro::net::tcp::client, io_status>>
    {
        auto [status, socket, endpoint] =
            co_await m_scheduler->accept(m_accept_socket, timeout, m_stop_source.get_token());
        if (!status.is_ok())
        {
            co_return unexpected<io_status>{status};
        }

        co_return tcp::client{m_scheduler, std::move(socket), endpoint};
    };

    /**
     * @return The tcp accept socket this server is using.
     * @{
     **/
    [[nodiscard]] auto accept_socket() -> net::socket& { return m_accept_socket; }
    [[nodiscard]] auto accept_socket() const -> const net::socket& { return m_accept_socket; }
    /** @} */

    auto shutdown()
    {
        m_stop_source.request_stop();
        m_accept_socket.shutdown(net::socket::shutdown_how::read_write);
    }

    /**
     * Accepts an incoming tcp client connection.
     * @return The newly connected tcp client connection.
     */
    auto accept_now() -> expected<coro::net::tcp::client, io_status>
    {
        auto client_endpoint     = socket_address::make_uninitialised();
        auto [sockaddr, socklen] = client_endpoint.native_mutable_data();

        net::native_handle_t sock_fd = ::accept(m_accept_socket.native_handle(), sockaddr, socklen);
        if (sock_fd == net::invalid_native_handle)
        {
            return unexpected<io_status>{make_io_status_from_native(net::detail::last_native_error())};
        }

        return tcp::client{m_scheduler, net::socket{sock_fd}, client_endpoint};
    };

private:
    friend client;
    /// The io scheduler for awaiting new connections.
    coro::scheduler* m_scheduler{nullptr};
    /// The bind and listen options for this server.
    options m_options;
    /// The socket for accepting new tcp connections on.
    net::socket m_accept_socket{net::invalid_native_handle};
    /// Stop signal to cancel a pending accept operation.
    std::stop_source m_stop_source{};
};

} // namespace coro::net::tcp
