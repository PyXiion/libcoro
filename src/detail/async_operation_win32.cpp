#include "coro/detail/async_operation.hpp"

#include <algorithm>
#include <limits>
#include <windows.h>

namespace coro::detail
{

auto perform_win32(async_operation& op) -> bool
{
    auto fd     = static_cast<SOCKET>(op.m_fd);
    auto length = static_cast<int>(std::min(op.m_length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    int  result{SOCKET_ERROR};
    switch (op.m_kind)
    {
        case async_op_kind::read:
            result = ::recv(fd, static_cast<char*>(op.m_buffer), length, 0);
            break;
        case async_op_kind::write:
            result = ::send(fd, static_cast<const char*>(op.m_buffer), length, 0);
            break;
        case async_op_kind::recv_from:
        {
            auto [address, address_length] = op.m_address->native_mutable_data();
            *address_length                = sizeof(sockaddr_storage);
            result = ::recvfrom(fd, static_cast<char*>(op.m_buffer), length, 0, address, address_length);
            break;
        }
        case async_op_kind::send_to:
        {
            auto [address, address_length] = op.m_address->data();
            result = ::sendto(fd, static_cast<const char*>(op.m_buffer), length, 0, address, address_length);
            break;
        }
        case async_op_kind::accept:
        {
            auto [address, address_length] = op.m_address->native_mutable_data();
            *address_length                = sizeof(sockaddr_storage);
            auto accepted                  = ::accept(fd, address, address_length);
            if (accepted != INVALID_SOCKET)
            {
                u_long non_blocking{1};
                int    error{0};
                if (::ioctlsocket(accepted, FIONBIO, &non_blocking) == SOCKET_ERROR)
                {
                    error = ::WSAGetLastError();
                }
                else if (!::SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0))
                {
                    error = static_cast<int>(::GetLastError());
                }
                if (error != 0)
                {
                    ::closesocket(accepted);
                    op.m_status = net::make_io_status_from_native(error);
                    return true;
                }
                op.m_accepted = static_cast<net::native_handle_t>(accepted);
                op.m_status   = {};
                return true;
            }
            break;
        }
        case async_op_kind::connect:
        case async_op_kind::timer:
            return false;
    }

    if (result >= 0)
    {
        op.m_bytes  = static_cast<std::size_t>(result);
        op.m_status = {
            op.m_kind == async_op_kind::read && result == 0 && op.m_length > 0 ? net::io_status::kind::closed
                                                                               : net::io_status::kind::ok};
        return true;
    }
    auto error = ::WSAGetLastError();
    if (error == WSAEWOULDBLOCK)
    {
        return false;
    }
    if (op.m_kind == async_op_kind::recv_from && error == WSAEMSGSIZE)
    {
        op.m_bytes  = op.m_length;
        op.m_status = {};
        return true;
    }
    op.m_status = net::make_io_status_from_native(error);
    return true;
}

} // namespace coro::detail
