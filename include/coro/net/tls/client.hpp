#ifdef LIBCORO_FEATURE_TLS

    #pragma once

    #include "coro/concepts/buffer.hpp"
    #include "coro/net/io_status.hpp"
    #include "coro/net/socket.hpp"
    #include "coro/net/socket_address.hpp"
    #include "coro/net/tls/connection_status.hpp"
    #include "coro/net/tls/context.hpp"
    #include "coro/scheduler.hpp"
    #include "coro/task.hpp"

    #include <openssl/ssl.h>

    #include <atomic>
    #include <chrono>
    #include <memory>
    #include <optional>
    #include <stop_token>

namespace coro::net::tls
{
class server;

/**
 * A TLS connection driven by the scheduler's completion operations.
 * Read and write operations must not run concurrently on the same client: they share one SSL object.
 */
class client
{
public:
    /**
     * Creates a new tls client that can connect to an ip address + port.
     * @param scheduler The io scheduler to drive the tls client.
     * @param tls_ctx The tls context.
     * @param endpoint The address and port to connect to.
     */
    explicit client(
        std::unique_ptr<coro::scheduler>& scheduler,
        std::shared_ptr<context>          tls_ctx,
        const net::socket_address&        endpoint);
    client(const client&) = delete;
    client(client&& other) noexcept;
    auto operator=(const client&) noexcept -> client& = delete;
    auto operator=(client&& other) noexcept -> client&;
    /**
     * If shutdown() was not awaited, blocks the calling thread with coro::sync_wait() for up to 30 seconds while
     * the TLS shutdown completes. That shutdown needs the scheduler's event loop, so destroying an un-shut-down
     * client on the scheduler's event loop thread (inline execution or a manual process_events() caller) deadlocks.
     */
    ~client();

    /**
     * @return The tcp socket this client is using.
     * @{
     **/
    [[nodiscard]] auto socket() -> net::socket& { return m_socket; }
    [[nodiscard]] auto socket() const -> const net::socket& { return m_socket; }
    /** @} */

    /**
     * Connects to the address+port and completes the TLS handshake.
     * Once called, subsequent calls return the cached result rather than reconnecting.
     * @param timeout Maximum total time for the TCP connection and TLS handshake; zero waits indefinitely.
     * @param stop_token Requests cancellation; cancellation is cached as connection_status::error.
     * @return The result status of trying to connect.
     */
    auto connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}, std::stop_token stop_token = {})
        -> coro::task<connection_status>;

    /**
     * Attempts to asynchronously read data into the provided buffer.
     * The operation may read fewer bytes than requested; the returned span references the filled prefix.
     * @note Operations on the same client must not run concurrently.
     * @see coro::net::read_exact()
     * @param buffer Destination buffer whose elements are one byte.
     * @param timeout Maximum time to wait for the read; zero waits indefinitely.
     * @param stop_token Requests cancellation of the operation.
     * @return The operation status and a span pointing to the read part of the buffer.
     * Unexpected TCP EOF reports tls_error; authenticated TLS close_notify reports closed.
     */
    template<
        concepts::mutable_buffer buffer_type,
        typename element_type = typename concepts::mutable_buffer_traits<buffer_type>::element_type>
    auto read_some(
        buffer_type&                    buffer,
        const std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token                 stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
    {
        static_assert(sizeof(element_type) == 1);

        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, {}};
        }
        auto [status, bytes_read] =
            co_await read_some_impl(std::as_writable_bytes(std::span{buffer}), make_deadline(timeout), stop_token);
        co_return {status, std::span<element_type>{buffer.data(), bytes_read}};
    }

    /**
     * Attempts to asynchronously write data from the provided buffer.
     * The returned span always refers to the unsent portion of the original buffer.
     * Plaintext accepted by SSL counts as sent even if flushing its ciphertext subsequently times out;
     * pending ciphertext is retained and flushed by the next operation.
     * @note Operations on the same client must not run concurrently.
     * @see coro::net::write_all()
     * @param buffer Source buffer whose elements are one byte.
     * @param timeout Maximum time to wait for the write; zero waits indefinitely.
     * @param stop_token Requests cancellation of the operation.
     * @return The operation status and a span pointing to the unsent portion; empty if all data was sent.
     */
    template<
        concepts::const_buffer buffer_type,
        typename element_type = typename concepts::const_buffer_traits<buffer_type>::element_type>
    auto write_some(
        const buffer_type&              buffer,
        const std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token                 stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
    {
        static_assert(sizeof(element_type) == 1);

        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, {}};
        }
        auto [status, bytes_written] =
            co_await write_some_impl(std::as_bytes(std::span{buffer}), make_deadline(timeout), stop_token);
        co_return {status, std::span<element_type>{buffer.data() + bytes_written, buffer.size() - bytes_written}};
    }

    /**
     * Shuts down the tls client with a 30 second timeout.
     * Always co_await this before destroying the client: otherwise the destructor blocks on the shutdown, which
     * deadlocks when the destructor runs on the scheduler's event loop thread (see ~client()).
     * @return Task.
     */
    auto shutdown() -> coro::task<void> { co_await shutdown(std::chrono::seconds{30}); }

    template<typename rep, typename period>
    auto shutdown(std::chrono::duration<rep, period> timeout) -> coro::task<void>
    {
        if (m_shutdown.exchange(true, std::memory_order::acq_rel))
        {
            co_return;
        }

        if (m_tls_info.m_tls_ptr != nullptr && !m_tls_info.m_tls_error)
        {
            co_await tls_shutdown_and_free(std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
        }
    }

private:
    using deadline = std::chrono::steady_clock::time_point;

    static auto make_deadline(std::chrono::milliseconds timeout) -> deadline;

    auto handshake(deadline deadline, std::stop_token stop_token) -> coro::task<io_status>;

    template<typename ssl_call_type>
    auto drive(ssl_call_type ssl_call, deadline deadline, std::stop_token stop_token, bool shutting_down = false)
        -> coro::task<std::pair<int, io_status>>;
    auto flush_ciphertext(deadline deadline, std::stop_token stop_token) -> coro::task<io_status>;
    auto read_some_impl(std::span<std::byte> buffer, deadline deadline, std::stop_token stop_token)
        -> coro::task<std::pair<io_status, std::size_t>>;
    auto write_some_impl(std::span<const std::byte> buffer, deadline deadline, std::stop_token stop_token)
        -> coro::task<std::pair<io_status, std::size_t>>;
    auto tls_shutdown_and_free(std::chrono::milliseconds timeout) -> coro::task<void>;

    struct tls_deleter
    {
        auto operator()(SSL* ssl) const -> void { SSL_free(ssl); }
    };

    using tls_unique_ptr = std::unique_ptr<SSL, tls_deleter>;

    enum class tls_connection_type
    {
        connect,
        accept
    };

    struct tls_info
    {
        tls_connection_type          m_tls_connection_type{tls_connection_type::connect};
        tls_unique_ptr               m_tls_ptr{nullptr};
        bool                         m_tls_error{false};
        std::unique_ptr<std::byte[]> m_ciphertext_buffer{nullptr};
        std::size_t                  m_ciphertext_offset{0};
        std::size_t                  m_ciphertext_size{0};
    };

    friend server;
    client(
        coro::scheduler*           scheduler,
        std::shared_ptr<context>   tls_ctx,
        net::socket                socket,
        const net::socket_address& endpoint);

    coro::scheduler*                 m_scheduler{nullptr};
    std::shared_ptr<context>         m_tls_ctx{nullptr};
    net::socket_address              m_endpoint;
    net::socket                      m_socket{net::invalid_native_handle};
    std::optional<connection_status> m_connect_status{std::nullopt};
    tls_info                         m_tls_info{};
    std::atomic<bool>                m_shutdown{false};
};

} // namespace coro::net::tls

#endif // #ifdef LIBCORO_FEATURE_TLS
