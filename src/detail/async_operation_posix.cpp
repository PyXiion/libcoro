#include "coro/detail/async_operation.hpp"
#include "coro/platform.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace coro::detail
{

auto perform_posix(async_operation& op) -> bool
{
    for (;;)
    {
        ssize_t result{-1};
        switch (op.m_kind)
        {
            case async_op_kind::read:
                result = ::recv(op.m_fd, op.m_buffer, op.m_length, 0);
                break;
            case async_op_kind::write:
                result = ::send(op.m_fd, op.m_buffer, op.m_length, 0);
                break;
            case async_op_kind::recv_from:
            {
                auto [address, length] = op.m_address->native_mutable_data();
                result                 = ::recvfrom(op.m_fd, op.m_buffer, op.m_length, 0, address, length);
                break;
            }
            case async_op_kind::send_to:
            {
                auto [address, length] = op.m_address->data();
                result                 = ::sendto(op.m_fd, op.m_buffer, op.m_length, 0, address, length);
                break;
            }
            case async_op_kind::accept:
            {
                auto [address, length] = op.m_address->native_mutable_data();
#if defined(CORO_PLATFORM_LINUX)
                auto accepted = ::accept4(op.m_fd, address, length, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
                auto accepted = ::accept(op.m_fd, address, length);
                if (accepted != -1)
                {
                    auto flags            = ::fcntl(accepted, F_GETFL, 0);
                    auto descriptor_flags = ::fcntl(accepted, F_GETFD, 0);
                    if (flags == -1 || descriptor_flags == -1 || ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1 ||
                        ::fcntl(accepted, F_SETFD, descriptor_flags | FD_CLOEXEC) == -1)
                    {
                        auto error = errno;
                        ::close(accepted);
                        op.m_status = net::make_io_status_from_native(error);
                        return true;
                    }
                }
#endif
                if (accepted != -1)
                {
                    op.m_accepted = accepted;
                    op.m_status   = {};
                    return true;
                }
                break;
            }
            case async_op_kind::connect:
            {
                if (!op.m_connect_started)
                {
                    auto [address, length] = op.m_address->data();
                    result                 = ::connect(op.m_fd, address, length);
                    // An interrupted connect keeps establishing in the background, retrying would fail with EALREADY.
                    if (result == -1 && (errno == EINPROGRESS || errno == EINTR || errno == EALREADY))
                    {
                        op.m_connect_started = true;
                        return false;
                    }
                }
                else
                {
                    int       error{0};
                    socklen_t length{sizeof(error)};
                    if (::getsockopt(op.m_fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0)
                    {
                        op.m_status = net::make_io_status_from_native(error);
                        return true;
                    }
                }
                if (result == 0)
                {
                    op.m_status = {};
                    return true;
                }
                break;
            }
            case async_op_kind::timer:
            case async_op_kind::poll:
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
        auto error = errno;
        if (error == EINTR)
        {
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK)
        {
            return false;
        }
        op.m_status = net::make_io_status_from_native(error);
        return true;
    }
}

} // namespace coro::detail
