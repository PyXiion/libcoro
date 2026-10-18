#ifdef LIBCORO_FEATURE_TLS

    #pragma once

    #include "coro/expected.hpp"
    #include "coro/net/ip_address.hpp"
    #include "coro/net/socket.hpp"
    #include "coro/net/tls/client.hpp"
    #include "coro/task.hpp"

    #include <stop_token>

namespace coro
{
class scheduler;
} // namespace coro

namespace coro::net::tls
{
class context;

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
        std::shared_ptr<context>          tls_ctx,
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
     * Accepts an incoming connection and completes its TLS handshake.
     * @param timeout Maximum time to wait for a connection; zero waits indefinitely.
     * @param handshake_timeout Maximum time to complete the TLS handshake; zero waits indefinitely.
     * @return The connected TLS client or the accept/handshake failure status.
     */
    auto accept(
        std::chrono::milliseconds timeout           = std::chrono::milliseconds{0},
        std::chrono::milliseconds handshake_timeout = std::chrono::seconds{30})
        -> coro::task<expected<tls::client, io_status>>;

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

private:
    /// The io scheduler for awaiting new connections.
    coro::scheduler* m_scheduler{nullptr};
    // The tls context.
    std::shared_ptr<context> m_tls_ctx{nullptr};
    /// The bind and listen options for this server.
    options m_options;
    /// The socket for accepting new tcp connections on.
    net::socket m_accept_socket{net::invalid_native_handle};
    /// Stop signal to cancel a pending accept or TLS handshake.
    std::stop_source m_stop_source{};
};

} // namespace coro::net::tls

#endif // #ifdef LIBCORO_FEATURE_TLS
