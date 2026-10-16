#ifdef LIBCORO_FEATURE_TLS

    #include "coro/net/tls/client.hpp"
    #include "coro/sync_wait.hpp"

    #include <openssl/bio.h>
    #include <openssl/err.h>

    #include <new>

namespace coro::net::tls
{
namespace
{
constexpr std::size_t ciphertext_buffer_size = 16 * 1024 + 1024;

auto remaining_timeout(std::chrono::steady_clock::time_point deadline) -> std::chrono::milliseconds
{
    if (deadline == std::chrono::steady_clock::time_point::max())
    {
        return std::chrono::milliseconds{0};
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return std::chrono::milliseconds{-1};
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

auto handshake_status(io_status status) -> connection_status
{
    switch (status.type)
    {
        case io_status::kind::ok:
            return connection_status::connected;
        case io_status::kind::timeout:
            return connection_status::timeout;
        case io_status::kind::closed:
        case io_status::kind::connection_reset:
            return connection_status::unexpected_close;
        case io_status::kind::tls_error:
            return connection_status::handshake_failed;
        default:
            return connection_status::error;
    }
}
} // namespace

client::client(
    std::unique_ptr<coro::scheduler>& scheduler, std::shared_ptr<context> tls_ctx, const net::socket_address& endpoint)
    : m_scheduler(scheduler.get()),
      m_tls_ctx(std::move(tls_ctx)),
      m_endpoint(endpoint),
      m_socket(
          net::make_socket(
              net::socket::options{net::socket::type_t::tcp, net::socket::blocking_t::no}, endpoint.domain()))
{
    if (m_scheduler == nullptr)
    {
        throw std::runtime_error{"tls::client cannot have nullptr scheduler"};
    }
    if (m_tls_ctx == nullptr)
    {
        throw std::runtime_error{"tls::client cannot have nullptr tls_ctx"};
    }
}

client::client(
    coro::scheduler*           scheduler,
    std::shared_ptr<context>   tls_ctx,
    net::socket                socket,
    const net::socket_address& endpoint)
    : m_scheduler(scheduler),
      m_tls_ctx(std::move(tls_ctx)),
      m_endpoint(endpoint),
      m_socket(std::move(socket)),
      m_connect_status(connection_status::connected),
      m_tls_info{tls_connection_type::accept}
{
    m_socket.blocking(net::socket::blocking_t::no);
}

client::client(client&& other) noexcept
    : m_scheduler(std::exchange(other.m_scheduler, nullptr)),
      m_tls_ctx(std::move(other.m_tls_ctx)),
      m_endpoint(std::move(other.m_endpoint)),
      m_socket(std::move(other.m_socket)),
      m_connect_status(std::exchange(other.m_connect_status, std::nullopt)),
      m_tls_info(std::move(other.m_tls_info)),
      m_shutdown(other.m_shutdown.exchange(true, std::memory_order::acq_rel))
{
}

client::~client()
{
    if (!m_shutdown.load(std::memory_order::acquire))
    {
        coro::sync_wait(shutdown(std::chrono::seconds{30}));
    }
}

auto client::operator=(client&& other) noexcept -> client&
{
    if (std::addressof(other) != this)
    {
        m_scheduler      = std::exchange(other.m_scheduler, nullptr);
        m_tls_ctx        = std::move(other.m_tls_ctx);
        m_endpoint       = std::move(other.m_endpoint);
        m_socket         = std::move(other.m_socket);
        m_connect_status = std::exchange(other.m_connect_status, std::nullopt);
        m_tls_info       = std::move(other.m_tls_info);
        m_shutdown.store(other.m_shutdown.exchange(true, std::memory_order::acq_rel), std::memory_order::release);
    }
    return *this;
}

auto client::make_deadline(std::chrono::milliseconds timeout) -> deadline
{
    return timeout.count() > 0 ? std::chrono::steady_clock::now() + timeout : deadline::max();
}

auto client::connect(std::chrono::milliseconds timeout, std::stop_token stop_token) -> coro::task<connection_status>
{
    if (m_connect_status.has_value())
    {
        co_return *m_connect_status;
    }
    if (m_tls_ctx == nullptr)
    {
        m_connect_status = connection_status::context_required;
        co_return *m_connect_status;
    }

    const auto deadline = make_deadline(timeout);
    const auto status   = co_await m_scheduler->connect(m_socket, m_endpoint, timeout, stop_token);
    if (!status.is_ok())
    {
        m_connect_status = status.is_timeout() ? connection_status::timeout : connection_status::error;
    }
    else
    {
        m_connect_status = handshake_status(co_await handshake(deadline, stop_token));
    }
    co_return *m_connect_status;
}

auto client::flush_ciphertext(deadline deadline, std::stop_token stop_token) -> coro::task<io_status>
{
    auto* wbio = SSL_get_wbio(m_tls_info.m_tls_ptr.get());
    auto& info = m_tls_info;

    while (info.m_ciphertext_offset < info.m_ciphertext_size || BIO_ctrl_pending(wbio) > 0)
    {
        if (stop_token.stop_requested())
        {
            co_return io_status{io_status::kind::cancelled};
        }
        const auto timeout = remaining_timeout(deadline);
        if (timeout.count() < 0)
        {
            co_return io_status{io_status::kind::timeout};
        }

        if (info.m_ciphertext_offset == info.m_ciphertext_size)
        {
            const auto bytes = BIO_read(wbio, info.m_ciphertext_buffer.get(), ciphertext_buffer_size);
            if (bytes <= 0)
            {
                info.m_tls_error = true;
                co_return io_status{io_status::kind::tls_error, SSL_ERROR_SSL};
            }
            info.m_ciphertext_offset = 0;
            info.m_ciphertext_size   = static_cast<std::size_t>(bytes);
        }

        const auto pending = std::span<const std::byte>{
            info.m_ciphertext_buffer.get() + info.m_ciphertext_offset,
            info.m_ciphertext_size - info.m_ciphertext_offset};
        auto [status, bytes_written] = co_await m_scheduler->write_some(m_socket, pending, timeout, stop_token);
        info.m_ciphertext_offset += bytes_written;
        if (!status.is_ok())
        {
            co_return status;
        }
        if (bytes_written == 0)
        {
            co_return io_status{io_status::kind::closed};
        }
    }
    info.m_ciphertext_offset = 0;
    info.m_ciphertext_size   = 0;
    co_return io_status{io_status::kind::ok};
}

template<typename ssl_call_type>
auto client::drive(ssl_call_type ssl_call, deadline deadline, std::stop_token stop_token, bool shutting_down)
    -> coro::task<std::pair<int, io_status>>
{
    auto* tls = m_tls_info.m_tls_ptr.get();
    if (tls == nullptr)
    {
        co_return {0, io_status{io_status::kind::closed}};
    }

    // A previous operation may have timed out after removing ciphertext from the BIO.
    auto status = co_await flush_ciphertext(deadline, stop_token);
    if (!status.is_ok())
    {
        co_return {0, status};
    }

    while (true)
    {
        if (stop_token.stop_requested())
        {
            co_return {0, io_status{io_status::kind::cancelled}};
        }
        if (remaining_timeout(deadline).count() < 0)
        {
            co_return {0, io_status{io_status::kind::timeout}};
        }

        ERR_clear_error();
        const auto result = ssl_call();
        // Capture the thread-local OpenSSL error before BIO calls or coroutine suspension.
        const auto error = result > 0 || (shutting_down && result == 0) ? SSL_ERROR_NONE : SSL_get_error(tls, result);
        const auto empty_error_queue = ERR_peek_error() == 0;
        if (error != SSL_ERROR_NONE && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE &&
            error != SSL_ERROR_ZERO_RETURN && !(error == SSL_ERROR_SYSCALL && empty_error_queue))
        {
            m_tls_info.m_tls_error = true;
        }

        status = co_await flush_ciphertext(deadline, stop_token);
        if (!status.is_ok())
        {
            co_return {result, status};
        }
        if (result > 0)
        {
            co_return {result, io_status{io_status::kind::ok}};
        }
        if (shutting_down && result == 0)
        {
            // The first shutdown sends close_notify; the next call waits for the peer's reply.
            continue;
        }

        switch (error)
        {
            case SSL_ERROR_WANT_READ:
            {
                if (stop_token.stop_requested())
                {
                    co_return {result, io_status{io_status::kind::cancelled}};
                }
                const auto timeout = remaining_timeout(deadline);
                if (timeout.count() < 0)
                {
                    co_return {result, io_status{io_status::kind::timeout}};
                }
                auto [read_status, bytes_read] = co_await m_scheduler->read_some(
                    m_socket,
                    std::span<std::byte>{m_tls_info.m_ciphertext_buffer.get(), ciphertext_buffer_size},
                    timeout,
                    stop_token);
                if (!read_status.is_ok())
                {
                    co_return {result, read_status};
                }
                if (bytes_read == 0)
                {
                    co_return {result, io_status{io_status::kind::closed}};
                }

                std::size_t offset{0};
                while (offset < bytes_read)
                {
                    const auto bytes = BIO_write(
                        SSL_get_rbio(tls),
                        m_tls_info.m_ciphertext_buffer.get() + offset,
                        static_cast<int>(bytes_read - offset));
                    if (bytes <= 0)
                    {
                        m_tls_info.m_tls_error = true;
                        co_return {result, io_status{io_status::kind::tls_error, SSL_ERROR_SSL}};
                    }
                    offset += static_cast<std::size_t>(bytes);
                }
                break;
            }
            case SSL_ERROR_WANT_WRITE:
                break;
            case SSL_ERROR_ZERO_RETURN:
                co_return {result, io_status{io_status::kind::closed}};
            case SSL_ERROR_SYSCALL:
                if (empty_error_queue)
                {
                    co_return {result, io_status{io_status::kind::closed}};
                }
                [[fallthrough]];
            default:
                m_tls_info.m_tls_error = true;
                co_return {result, io_status{io_status::kind::tls_error, error}};
        }
    }
}

auto client::handshake(deadline deadline, std::stop_token stop_token) -> coro::task<io_status>
{
    auto& info     = m_tls_info;
    info.m_tls_ptr = tls_unique_ptr{SSL_new(m_tls_ctx->native_handle())};
    info.m_ciphertext_buffer.reset(new (std::nothrow) std::byte[ciphertext_buffer_size]);
    if (info.m_tls_ptr == nullptr || info.m_ciphertext_buffer == nullptr)
    {
        info.m_tls_error = true;
        co_return io_status{io_status::kind::tls_error};
    }

    auto* rbio = BIO_new(BIO_s_mem());
    auto* wbio = BIO_new(BIO_s_mem());
    if (rbio == nullptr || wbio == nullptr)
    {
        BIO_free(rbio);
        BIO_free(wbio);
        info.m_tls_error = true;
        co_return io_status{io_status::kind::tls_error};
    }
    BIO_set_mem_eof_return(rbio, -1);
    auto* tls = info.m_tls_ptr.get();
    SSL_set_bio(tls, rbio, wbio);
    if (info.m_tls_connection_type == tls_connection_type::connect)
    {
        SSL_set_connect_state(tls);
    }
    else
    {
        SSL_set_accept_state(tls);
    }

    auto [result, status] = co_await drive([tls]() -> int { return SSL_do_handshake(tls); }, deadline, stop_token);
    if (!status.is_ok())
    {
        // An incomplete handshake cannot be shut down as an established TLS connection.
        info.m_tls_ptr.reset();
    }
    co_return status;
}

auto client::read_some_impl(std::span<std::byte> buffer, deadline deadline, std::stop_token stop_token)
    -> coro::task<std::pair<io_status, std::size_t>>
{
    std::size_t bytes_read{0};
    auto [result, status] = co_await drive(
        [this, buffer, &bytes_read]() -> int
        { return SSL_read_ex(m_tls_info.m_tls_ptr.get(), buffer.data(), buffer.size(), &bytes_read); },
        deadline,
        stop_token);
    co_return {status, bytes_read};
}

auto client::write_some_impl(std::span<const std::byte> buffer, deadline deadline, std::stop_token stop_token)
    -> coro::task<std::pair<io_status, std::size_t>>
{
    std::size_t bytes_written{0};
    auto [result, status] = co_await drive(
        [this, buffer, &bytes_written]() -> int
        { return SSL_write_ex(m_tls_info.m_tls_ptr.get(), buffer.data(), buffer.size(), &bytes_written); },
        deadline,
        stop_token);
    co_return {status, bytes_written};
}

auto client::read_exact_impl(std::span<std::byte> buffer, std::chrono::milliseconds timeout, std::stop_token stop_token)
    -> coro::task<std::pair<io_status, std::span<std::byte>>>
{
    const auto deadline  = make_deadline(timeout);
    auto       remaining = buffer;
    while (!remaining.empty())
    {
        auto [status, bytes_read] = co_await read_some_impl(remaining, deadline, stop_token);
        remaining                 = remaining.subspan(bytes_read);
        if (!status.is_ok())
        {
            co_return {status, buffer.first(buffer.size() - remaining.size())};
        }
    }
    co_return {io_status{io_status::kind::ok}, buffer};
}

auto client::write_all_impl(
    std::span<const std::byte> buffer, std::chrono::milliseconds timeout, std::stop_token stop_token)
    -> coro::task<std::pair<io_status, std::span<const std::byte>>>
{
    const auto deadline  = make_deadline(timeout);
    auto       remaining = buffer;
    while (!remaining.empty())
    {
        auto [status, bytes_written] = co_await write_some_impl(remaining, deadline, stop_token);
        remaining                    = remaining.subspan(bytes_written);
        if (!status.is_ok())
        {
            co_return {status, remaining};
        }
    }
    co_return {io_status{io_status::kind::ok}, {}};
}

auto client::tls_shutdown_and_free(std::chrono::milliseconds timeout) -> coro::task<void>
{
    auto* tls = m_tls_info.m_tls_ptr.get();
    co_await drive([tls]() -> int { return SSL_shutdown(tls); }, make_deadline(timeout), {}, true);
}

} // namespace coro::net::tls

#endif // #ifdef LIBCORO_FEATURE_TLS
