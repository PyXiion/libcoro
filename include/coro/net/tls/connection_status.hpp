#ifdef LIBCORO_FEATURE_TLS

    #pragma once

    #include <string>

namespace coro::net::tls
{
enum class connection_status
{
    /// The tls connection was successful.
    connected,
    /// The connection hasn't been established yet, use connect() prior to the handshake().
    not_connected,
    /// The connection needs a coro::net::tls::context to perform the handshake.
    context_required,
    /// The handshake had an error.
    handshake_failed,
    /// The connection timed out.
    timeout,
    /// The socket was unexpectedly closed while attempting the handshake.
    unexpected_close,
    /// The given ip address could not be parsed or is invalid.
    invalid_ip_address,
    /// There was an unrecoverable error, use errno to get more information on the specific error.
    error
};

auto to_string(connection_status status) -> const std::string&;

} // namespace coro::net::tls

#endif // #ifdef LIBCORO_FEATURE_TLS
