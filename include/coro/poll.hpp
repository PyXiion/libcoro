#pragma once

#include <cstdint>
#include <string>

#include "coro/platform.hpp"

namespace coro
{
enum class poll_op : uint64_t
{
    /// Poll for read operations.
    read = 1,
    /// Poll for write operations.
    write = 2,
    /// Poll for read and write operations.
    read_write = 3
};

inline auto poll_op_readable(poll_op op) -> bool
{
    return (static_cast<uint64_t>(op) & static_cast<uint64_t>(poll_op::read));
}

inline auto poll_op_writeable(poll_op op) -> bool
{
    return (static_cast<uint64_t>(op) & static_cast<uint64_t>(poll_op::write));
}

auto to_string(poll_op op) -> const std::string&;

enum class poll_status
{
    /// The poll operation was was successful with a read-event.
    read,
    /// The poll operation was was successful with a write-event.
    write,
    /// The poll operation timed out.
    timeout,
    /// The file descriptor had an error while polling.
    error,
    /// The file descriptor has been closed by the remote or an internal error/close.
    closed,
    /// The poll operation was cancelled by a stop request.
    cancelled,
};

auto to_string(poll_status status) -> const std::string&;

} // namespace coro
