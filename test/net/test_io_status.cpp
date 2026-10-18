#include "catch_amalgamated.hpp"
#include "coro/net/detail/socket_api.hpp"
#include "coro/net/io_status.hpp"
#include <cerrno>

TEST_CASE("Mapping native codes", "[io_status]")
{
#if defined(CORO_PLATFORM_WINDOWS)
    auto [input_code, expected_kind] = GENERATE(
        table<int, coro::net::io_status::kind>({
            {0, coro::net::io_status::kind::ok},
            {WSAECONNREFUSED, coro::net::io_status::kind::connection_refused},
            {WSAECONNRESET, coro::net::io_status::kind::connection_reset},
            {WSAEWOULDBLOCK, coro::net::io_status::kind::would_block_or_try_again},
            {WSAEMSGSIZE, coro::net::io_status::kind::message_too_big},
            {WSA_OPERATION_ABORTED, coro::net::io_status::kind::cancelled},
            {123456, coro::net::io_status::kind::native} // Covers default case
        }));
#else
    auto [input_code, expected_kind] = GENERATE(
        table<int, coro::net::io_status::kind>({
            {0, coro::net::io_status::kind::ok},
            {EOF, coro::net::io_status::kind::closed},
            {ECONNREFUSED, coro::net::io_status::kind::connection_refused},
            {ECONNRESET, coro::net::io_status::kind::connection_reset},
            {EAGAIN, coro::net::io_status::kind::would_block_or_try_again},
            {EMSGSIZE, coro::net::io_status::kind::message_too_big},
            {123456, coro::net::io_status::kind::native} // Covers default case
        }));
#endif

    auto status = coro::net::make_io_status_from_native(input_code);

    CHECK(status.type == expected_kind);
    CHECK(status.native_code == input_code);
}