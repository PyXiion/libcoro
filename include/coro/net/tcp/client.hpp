#pragma once

#include "coro/concepts/buffer.hpp"
#include "coro/net/connect.hpp"
#include "coro/net/io_status.hpp"
#include "coro/net/ip_address.hpp"
#include "coro/net/socket.hpp"
#include "coro/net/socket_address.hpp"
#include "coro/scheduler.hpp"
#include "coro/task.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>

namespace coro::net::tcp
{
class server;

class client
{
public:
    /**
     * Creates a new tcp client that can connect to an ip address + port.
     * @param scheduler The io scheduler to drive the tcp client.
     * @param opts See client::options for more information.
     */
    explicit client(std::unique_ptr<coro::scheduler>& scheduler, net::socket_address endpoint);
    client(const client& other);
    client(client&& other) noexcept;
    auto operator=(const client& other) noexcept -> client&;
    auto operator=(client&& other) noexcept -> client&;
    ~client();

    /**
     * @return The tcp socket this client is using.
     * @{
     **/
    [[nodiscard]] auto socket() -> net::socket& { return m_socket; }
    [[nodiscard]] auto socket() const -> const net::socket& { return m_socket; }
    /** @} */

    /**
     * Connects to the address+port with the given timeout. Once connected calling this function
     * only returns the connected status, it will not reconnect.
     * @param timeout How long to wait for the connection to establish? Timeout of zero is indefinite.
     * @param stop_token Requests cancellation; cancellation is cached as connect_status::error.
     * @return The result status of trying to connect.
     */
    auto connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{0}, std::stop_token stop_token = {})
        -> coro::task<net::connect_status>;

    /**
     * Attempts to asynchronously read data from the socket into the provided buffer.
     *
     * The function may read fewer bytes than requested. In that case, the returned
     * status will be 'ok' and the returned span will reference the prefix of the
     * buffer that was filled with received data.
     *
     * @note
     *   This function is not safe to call concurrently from multiple coroutines
     *   on the same socket.
     *
     * @see read_exact()
     *
     * @param buffer Destination buffer to read data into.
     * @param timeout Maximum time to wait for the read to complete.
     *                Use 0 for infinite timeout.
     * @param stop_token Requests cancellation of the operation.
     * @return A pair of:
     *          - status of the operation
     *          - span pointing to the read part of buffer
     */
    template<
        concepts::mutable_buffer buffer_type,
        typename element_type = typename concepts::mutable_buffer_traits<buffer_type>::element_type>
    auto read_some(
        buffer_type&                    buffer,
        const std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token                 stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
    {
        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, {}};
        }
        auto [status, bytes_read] =
            co_await m_scheduler->read_some(m_socket, std::as_writable_bytes(std::span{buffer}), timeout, stop_token);
        co_return {status, std::span<element_type>{buffer.data(), bytes_read}};
    }

    /**
     * Asynchronously reads data from the socket into the provided buffer.
     *
     * This function repeatedly invokes read_some() until either:
     * - the whole buffer is filled, or
     * - an error or timeout occurs.
     *
     * The timeout is treated as a soft overall deadline for the entire operation.
     *
     * @note
     *   This function is not safe to call concurrently from multiple coroutines
     *   on the same socket.
     *
     * @see read_some()
     *
     * @param buffer
     *        Destination buffer to read data into.
     * @param timeout
     *        Maximum total time allowed for the operation.
     *        A value of 0 results in infinite timeout.
     * @param stop_token Requests cancellation, preserving any data already read.
     * @return A pair of:
     *         - status of the operation
     *         - span pointing to the read part of buffer
     */
    template<
        concepts::mutable_buffer buffer_type,
        typename element_type = typename concepts::mutable_buffer_traits<buffer_type>::element_type>
    auto read_exact(
        buffer_type&                    buffer,
        const std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token                 stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
    {
        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, {}};
        }
        auto [status, buf] = co_await read_exact_impl(std::as_writable_bytes(std::span{buffer}), timeout, stop_token);
        co_return {status, std::span<element_type>{reinterpret_cast<element_type*>(buf.data()), buf.size()}};
    }

    /**
     * Attempts to asynchronously write data from the provided buffer to the socket.
     *
     * The operation may write only a portion of the provided buffer.
     * In that case, the returned status will be 'ok' and the returned span
     * will reference the portion of the original buffer that was not sent.
     *
     * @note
     * - The returned span always refers to the original buffer.
     * - This function is not safe to call concurrently from multiple coroutines
     *   on the same socket.
     *
     * @see write_all()
     * @param buffer
     *        Buffer containing the data to write.
     * @param timeout
     *        Maximum time to wait for the write to complete.
     *        A value of 0 results in infinite timeout.
     * @param stop_token Requests cancellation of the operation.
     * @return A pair of:
     *         - status of the operation
     *         - span pointing to the unsent portion of the buffer;
     *           empty if all data was sent successfully
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
            co_await m_scheduler->write_some(m_socket, std::as_bytes(std::span{buffer}), timeout, stop_token);
        co_return {status, std::span<element_type>{buffer.data() + bytes_written, buffer.size() - bytes_written}};
    }

    /**
     * Asynchronously writes the entire contents of the provided buffer to the socket.
     *
     * This function repeatedly invokes write_some() until either:
     * - all bytes have been sent successfully, or
     * - an error or timeout occurs.
     *
     * The timeout is treated as a soft overall deadline for the entire operation.
     *
     * @note
     *   This function is not safe to call concurrently from multiple coroutines
     *   on the same socket.
     *
     * @see write_some()
     *
     * @param buffer
     *        The data to write to the socket.
     * @param timeout
     *        Maximum total time allowed for the operation.
     *        A value of 0 results in infinite timeout.
     * @param stop_token Requests cancellation, preserving the unsent portion of the buffer.
     * @return A pair of:
     *         - status of the operation
     *         - span pointing to the unsent portion of the buffer;
     *           empty if all data was sent successfully
     */
    template<
        concepts::const_buffer buffer_type,
        typename element_type = typename concepts::const_buffer_traits<buffer_type>::element_type>
    auto write_all(
        const buffer_type&              buffer,
        const std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token                 stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
    {
        static_assert(sizeof(element_type) == 1);

        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, {}};
        }
        auto [status, buf] = co_await write_all_impl(std::as_bytes(std::span{buffer}), timeout, stop_token);
        co_return {status, std::span<element_type>{reinterpret_cast<element_type*>(buf.data()), buf.size()}};
    }

private:
    auto read_exact_impl(
        std::span<std::byte> buffer, const std::chrono::milliseconds timeout, std::stop_token stop_token)
        -> coro::task<std::pair<io_status, std::span<std::byte>>>
    {
        const auto           start_time = std::chrono::steady_clock::now();
        std::span<std::byte> remaining  = buffer;

        while (!remaining.empty())
        {
            std::chrono::milliseconds remaining_timeout{0};
            if (timeout.count() > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);

                if (elapsed >= timeout)
                {
                    // Returning read prefix of the span
                    co_return {
                        io_status{io_status::kind::timeout}, buffer.subspan(0, buffer.size() - remaining.size())};
                }
                remaining_timeout = timeout - elapsed;
            }

            auto [status, bytes_read] =
                co_await m_scheduler->read_some(m_socket, remaining, remaining_timeout, stop_token);
            remaining = remaining.subspan(bytes_read);

            if (!status.is_ok())
            {
                co_return {status, buffer.subspan(0, buffer.size() - remaining.size())};
            }
        }

        co_return {io_status{io_status::kind::ok}, buffer};
    }

    auto write_all_impl(
        std::span<const std::byte> buffer, const std::chrono::milliseconds timeout, std::stop_token stop_token)
        -> coro::task<std::pair<io_status, std::span<const std::byte>>>
    {
        const auto                 start_time = std::chrono::steady_clock::now();
        std::span<const std::byte> remaining  = buffer;

        while (!remaining.empty())
        {
            std::chrono::milliseconds remaining_timeout{0};
            if (timeout.count() > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);

                if (elapsed >= timeout)
                {
                    co_return {io_status{io_status::kind::timeout}, remaining};
                }
                remaining_timeout = timeout - elapsed;
            }

            auto [status, bytes_written] =
                co_await m_scheduler->write_some(m_socket, remaining, remaining_timeout, stop_token);
            remaining = remaining.subspan(bytes_written);

            if (!status.is_ok())
            {
                co_return {status, remaining};
            }
        }

        co_return {io_status{io_status::kind::ok}, {}};
    }

private:
    /// The tcp::server creates already connected clients and provides a tcp socket pre-built.
    friend server;
    client(coro::scheduler* scheduler, net::socket socket, const net::socket_address& endpoint);

    /// The scheduler that will drive this tcp client.
    coro::scheduler* m_scheduler{nullptr};
    /// Options for what server to connect to.
    socket_address m_endpoint;
    /// The tcp socket.
    net::socket m_socket{-1};
    /// Cache the status of the connect call in the event the user calls connect() again.
    std::optional<net::connect_status> m_connect_status{std::nullopt};
};

} // namespace coro::net::tcp
