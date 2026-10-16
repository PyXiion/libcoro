#ifdef LIBCORO_FEATURE_TLS

    #include "coro/net/tls/server.hpp"

    #include "coro/scheduler.hpp"

namespace coro::net::tls
{
server::server(
    std::unique_ptr<coro::scheduler>& scheduler,
    std::shared_ptr<context>          tls_ctx,
    const net::socket_address&        endpoint,
    options                           opts)
    : m_scheduler(scheduler.get()),
      m_tls_ctx(std::move(tls_ctx)),
      m_options(std::move(opts)),
      m_accept_socket(
          net::make_accept_socket(
              net::socket::options{net::socket::type_t::tcp, net::socket::blocking_t::no}, endpoint, m_options.backlog))
{
    if (m_scheduler == nullptr)
    {
        throw std::runtime_error{"tls::server cannot have a nullptr scheduler"};
    }

    if (m_tls_ctx == nullptr)
    {
        throw std::runtime_error{"tls::server cannot have a nullptr tls_ctx"};
    }
}

server::server(server&& other)
    : m_scheduler(std::exchange(other.m_scheduler, nullptr)),
      m_tls_ctx(std::move(other.m_tls_ctx)),
      m_options(std::move(other.m_options)),
      m_accept_socket(std::move(other.m_accept_socket)),
      m_stop_source(std::move(other.m_stop_source))
{
}

auto server::operator=(server&& other) -> server&
{
    if (std::addressof(other) != this)
    {
        m_scheduler     = std::exchange(other.m_scheduler, nullptr);
        m_tls_ctx       = std::move(other.m_tls_ctx);
        m_options       = std::move(other.m_options);
        m_accept_socket = std::move(other.m_accept_socket);
        m_stop_source   = std::move(other.m_stop_source);
    }
    return *this;
}

auto server::accept(std::chrono::milliseconds timeout, std::chrono::milliseconds handshake_timeout)
    -> coro::task<expected<tls::client, io_status>>
{
    auto [status, socket, endpoint] = co_await m_scheduler->accept(m_accept_socket, timeout, m_stop_source.get_token());
    if (!status.is_ok())
    {
        co_return unexpected<io_status>{status};
    }

    auto tls_client = tls::client{m_scheduler, m_tls_ctx, std::move(socket), endpoint};
    status = co_await tls_client.handshake(client::make_deadline(handshake_timeout), m_stop_source.get_token());
    if (!status.is_ok())
    {
        co_return unexpected<io_status>{status};
    }
    co_return std::move(tls_client);
}

} // namespace coro::net::tls

#endif // #ifdef LIBCORO_FEATURE_TLS
