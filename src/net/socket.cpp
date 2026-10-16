#include "coro/net/socket.hpp"
#include "coro/net/socket_address.hpp"

#include <stdexcept>

#if defined(CORO_PLATFORM_WINDOWS)
    #include <windows.h>
#endif

namespace coro::net
{
#if defined(CORO_PLATFORM_WINDOWS)
namespace
{
// Winsock stays initialized for the process lifetime: calling WSACleanup() from a static destructor could run
// before other static objects owning sockets are destroyed.
struct wsa_init
{
    wsa_init()
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw std::runtime_error{"Failed to initialize Winsock."};
        }
    }
};

auto ensure_wsa_init() -> void
{
    static wsa_init init;
}
} // namespace
#endif

auto socket::type_to_os(type_t type) -> int
{
    switch (type)
    {
        case type_t::udp:
            return SOCK_DGRAM;
        case type_t::tcp:
            return SOCK_STREAM;
        default:
            throw std::runtime_error{"Unknown socket::type_t."};
    }
}

socket::socket(const socket& other)
{
#if defined(CORO_PLATFORM_WINDOWS)
    WSAPROTOCOL_INFOW info;
    if (WSADuplicateSocketW(other.m_handle, GetCurrentProcessId(), &info) == 0)
    {
        m_handle =
            WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
    }
#else
    m_handle = dup(other.m_handle);
#endif
}

auto socket::operator=(const socket& other) noexcept -> socket&
{
    if (std::addressof(other) != this)
    {
        socket copy{other};
        this->close();
        this->m_handle = std::exchange(copy.m_handle, invalid_native_handle);
    }
    return *this;
}

auto socket::operator=(socket&& other) noexcept -> socket&
{
    if (std::addressof(other) != this)
    {
        this->close();
        m_handle = std::exchange(other.m_handle, invalid_native_handle);
    }

    return *this;
}

auto socket::blocking(blocking_t block) -> bool
{
    if (m_handle == invalid_native_handle)
    {
        return false;
    }

#if defined(CORO_PLATFORM_WINDOWS)
    u_long mode = (block == blocking_t::no) ? 1 : 0;
    return (ioctlsocket(m_handle, FIONBIO, &mode) == 0);
#else
    int flags = fcntl(m_handle, F_GETFL, 0);
    if (flags == -1)
    {
        return false;
    }

    // Add or subtract non-blocking flag.
    flags = (block == blocking_t::yes) ? flags & ~O_NONBLOCK : (flags | O_NONBLOCK);

    return (fcntl(m_handle, F_SETFL, flags) == 0);
#endif
}

auto socket::shutdown(shutdown_how how) -> bool
{
    if (m_handle != invalid_native_handle)
    {
        int h{0};
        switch (how)
        {
            case shutdown_how::read:
#if defined(CORO_PLATFORM_WINDOWS)
                h = SD_RECEIVE;
#else
                h = SHUT_RD;
#endif
                break;
            case shutdown_how::write:
#if defined(CORO_PLATFORM_WINDOWS)
                h = SD_SEND;
#else
                h = SHUT_WR;
#endif
                break;
            case shutdown_how::read_write:
#if defined(CORO_PLATFORM_WINDOWS)
                h = SD_BOTH;
#else
                h = SHUT_RDWR;
#endif
                break;
        }

        return (::shutdown(m_handle, h) == 0);
    }
    return false;
}

auto socket::close() -> void
{
    if (m_handle != invalid_native_handle)
    {
#if defined(CORO_PLATFORM_WINDOWS)
        closesocket(m_handle);
#else
        ::close(m_handle);
#endif
        m_handle = invalid_native_handle;
    }
}

auto make_socket(const socket::options& opts, domain_t domain) -> socket
{
#if defined(CORO_PLATFORM_WINDOWS)
    ensure_wsa_init();
    socket s{WSASocketW(static_cast<int>(domain), socket::type_to_os(opts.type), 0, nullptr, 0, WSA_FLAG_OVERLAPPED)};
#else
    socket s{::socket(static_cast<int>(domain), socket::type_to_os(opts.type), 0)};
#endif
    if (s.native_handle() == invalid_native_handle)
    {
        throw std::runtime_error{"Failed to create socket."};
    }

    if (opts.blocking == socket::blocking_t::no)
    {
        if (s.blocking(socket::blocking_t::no) == false)
        {
            throw std::runtime_error{"Failed to set socket to non-blocking mode."};
        }
    }

    return s;
}

auto make_accept_socket(const socket::options& opts, const net::socket_address&endpoint, int32_t backlog)
    -> socket
{
    socket s = make_socket(opts, endpoint.domain());

    int sock_opt{1};

#if defined(CORO_PLATFORM_WINDOWS)
    if (setsockopt(
            s.native_handle(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&sock_opt),
            sizeof(sock_opt)) < 0)
    {
        throw std::runtime_error{"Failed to setsockopt(SO_EXCLUSIVEADDRUSE)"};
    }
#else
    #if defined(CORO_PLATFORM_LINUX)
    // On Linux the address and port should be marked for reuse.
    if (setsockopt(s.native_handle(), SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt)) < 0)
    {
        throw std::runtime_error{"Failed to setsockopt(SO_REUSEADDR)"};
    }
#endif

    if (setsockopt(s.native_handle(), SOL_SOCKET, SO_REUSEPORT, &sock_opt, sizeof(sock_opt)) < 0)
    {
        throw std::runtime_error{"Failed to setsockopt(SO_REUSEPORT)"};
    }
#endif

    auto [sockaddr, socklen] = endpoint.data();

    if (bind(s.native_handle(), sockaddr, socklen) < 0)
    {
        throw std::runtime_error{"Failed to bind."};
    }

    if (opts.type == socket::type_t::tcp)
    {
        if (listen(s.native_handle(), backlog) < 0)
        {
            throw std::runtime_error{"Failed to listen."};
        }
    }

    return s;
}

} // namespace coro::net
