#pragma once

#include "coro/concepts/buffer.hpp"
#include "coro/net/io_status.hpp"
#include "coro/net/ip_address.hpp"
#include "coro/net/socket.hpp"
#include "coro/net/socket_address.hpp"
#include "coro/scheduler.hpp"
#include "coro/task.hpp"

#include <chrono>
#include <span>
#include <stop_token>

namespace coro::net::udp
{
class peer
{
public:
    /**
     * Creates a udp peer that can send packets but not receive them.  This udp peer will not explicitly
     * bind to a local ip+port.
     */
    explicit peer(std::unique_ptr<coro::scheduler>& scheduler, net::domain_t domain = net::domain_t::ipv4);

    /**
     * Creates a udp peer that can send and receive packets.  This peer will bind to the given ip_port.
     */
    explicit peer(std::unique_ptr<coro::scheduler>& scheduler, const net::socket_address& endpoint);

    peer(const peer&) = delete;
    peer(peer&&) noexcept;
    auto operator=(const peer&) -> peer& = delete;
    auto operator=(peer&&) noexcept -> peer&;
    ~peer() = default;

    /**
     * @return A reference to the underlying socket.
     */
    auto socket() noexcept -> net::socket& { return m_socket; }

    /**
     * @return A const reference to the underlying socket.
     */
    auto socket() const noexcept -> const net::socket& { return m_socket; }

    /**
     * @param address The peer to send the data to.
     * @param buffer The data to send.
     * @param timeout Maximum time to wait for completion, zero waits indefinitely.
     * @param stop_token Requests cancellation of the operation.
     * @return The status of operation
     */
    template<concepts::const_buffer buffer_type>
    auto write_to(
        const socket_address&     address,
        const buffer_type&        buffer,
        std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
        std::stop_token           stop_token = {}) -> coro::task<io_status>
    {
        if (buffer.empty())
        {
            co_return io_status{io_status::kind::ok};
        }

        auto [status, bytes_sent] =
            co_await m_scheduler->send_to(m_socket, address, std::as_bytes(std::span{buffer}), timeout, stop_token);
        co_return status;
    }

    /**
     * @param buffer The buffer to receive data into.
     * @param timeout Maximum time to wait for completion, zero waits indefinitely.
     * @param stop_token Requests cancellation of the operation.
     * @return The receive status, if ok then also the peer who sent the data and the data.
     *         The span view of the data will be set to the size of the received data, this will
     *         always start at the beggining of the buffer but depending on how large the data was
     *         it might not fill the entire buffer.
     *         A datagram larger than the buffer succeeds with the prefix that fits;
     *         the remaining bytes of that datagram are discarded.
     */
    template<concepts::mutable_buffer buffer_type>
    auto read_from(
        buffer_type&              buffer,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
        std::stop_token stop_token = {}) -> coro::task<std::tuple<io_status, socket_address, std::span<std::byte>>>
    {
        // The user must bind locally to be able to receive packets.
        if (!m_bound)
        {
            co_return {io_status{io_status::kind::udp_not_bound}, net::socket_address::make_uninitialised(), {}};
        }

        if (buffer.empty())
        {
            co_return {io_status{io_status::kind::ok}, net::socket_address::make_uninitialised(), {}};
        }

        auto bytes                          = std::as_writable_bytes(std::span{buffer});
        auto [status, bytes_read, endpoint] = co_await m_scheduler->recv_from(m_socket, bytes, timeout, stop_token);
        co_return {status, std::move(endpoint), bytes.first(bytes_read)};
    }

private:
    /// The scheduler that will drive this udp client.
    coro::scheduler* m_scheduler;
    /// The udp socket.
    net::socket m_socket{net::invalid_native_handle};
    /// Did the user request this udp socket is bound locally to receive packets?
    bool m_bound{false};
};

} // namespace coro::net::udp
