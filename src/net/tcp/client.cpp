#include "coro/net/tcp/client.hpp"

namespace coro::net::tcp
{
using namespace std::chrono_literals;

client::client(std::unique_ptr<coro::scheduler>& scheduler, net::socket_address endpoint)
    : m_scheduler(scheduler.get()),
      m_endpoint(std::move(endpoint)),
      m_socket(
          net::make_socket(net::socket::options{socket::type_t::tcp, net::socket::blocking_t::no}, endpoint.domain()))
{
    if (m_scheduler == nullptr)
    {
        throw std::runtime_error{"tcp::client cannot have nullptr scheduler"};
    }
}

client::client(coro::scheduler* scheduler, net::socket socket, const net::socket_address& endpoint)
    : m_scheduler(scheduler),
      m_endpoint(std::move(endpoint)),
      m_socket(std::move(socket)),
      m_connect_status(connect_status::connected)
{
    // scheduler is assumed good since it comes from a tcp::server.

    // Force the socket to be non-blocking.
    m_socket.blocking(coro::net::socket::blocking_t::no);
}

client::client(client&& other) noexcept
    : m_scheduler(other.m_scheduler),
      m_endpoint(std::move(other.m_endpoint)),
      m_socket(std::move(other.m_socket)),
      m_connect_status(std::exchange(other.m_connect_status, std::nullopt))
{
}

client::~client()
{
}

auto client::operator=(client&& other) noexcept -> client&
{
    if (std::addressof(other) != this)
    {
        m_scheduler      = std::exchange(other.m_scheduler, nullptr);
        m_endpoint       = std::move(other.m_endpoint);
        m_socket         = std::move(other.m_socket);
        m_connect_status = std::exchange(other.m_connect_status, std::nullopt);
    }
    return *this;
}

auto client::connect(std::chrono::milliseconds timeout, std::stop_token stop_token) -> coro::task<connect_status>
{
    // Only allow the user to connect per tcp client once, if they need to re-connect they should
    // make a new tcp::client.
    if (m_connect_status.has_value())
    {
        co_return m_connect_status.value();
    }

    auto status = co_await m_scheduler->connect(m_socket, m_endpoint, timeout, stop_token);
    switch (status.type)
    {
        case io_status::kind::ok:
            m_connect_status = connect_status::connected;
            break;
        case io_status::kind::timeout:
            m_connect_status = connect_status::timeout;
            break;
        case io_status::kind::connection_refused:
        default:
            m_connect_status = connect_status::error;
            break;
    }

    co_return m_connect_status.value();
}

} // namespace coro::net::tcp
