#pragma once

#include "coro/platform.hpp"

#include <cstdint>

#if defined(CORO_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace coro::net
{
#if defined(CORO_PLATFORM_WINDOWS)
using native_handle_t                                  = std::uintptr_t;
inline constexpr native_handle_t invalid_native_handle = ~native_handle_t{0};
#else
using native_handle_t                                  = int;
inline constexpr native_handle_t invalid_native_handle = -1;
#endif

namespace detail
{
inline auto last_native_error() noexcept -> int
{
#if defined(CORO_PLATFORM_WINDOWS)
    return WSAGetLastError();
#else
    return errno;
#endif
}
} // namespace detail
} // namespace coro::net
