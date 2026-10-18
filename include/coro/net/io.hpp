#pragma once

#include "coro/concepts/buffer.hpp"
#include "coro/net/io_status.hpp"
#include "coro/task.hpp"

#include <chrono>
#include <span>
#include <stop_token>
#include <utility>

namespace coro::net
{

/**
 * Reads until the buffer is filled or an error, cancellation or timeout occurs.
 * The stream must provide read_some(buffer, timeout, stop_token), returning a status and a filled prefix.
 * Keep the stream and buffer alive until completion; do not run concurrent operations on the stream.
 * @param buffer Destination buffer whose elements are one byte.
 * @param timeout Soft overall deadline for the operation; zero or negative waits indefinitely.
 * @param stop_token Requests cancellation, preserving any data already read.
 * @return The status and the filled prefix, including bytes received by an error-bearing operation.
 */
template<
    typename stream_type,
    concepts::mutable_buffer buffer_type,
    typename element_type = typename concepts::mutable_buffer_traits<buffer_type>::element_type>
auto read_exact(
    stream_type&              stream,
    buffer_type&              buffer,
    std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
    std::stop_token           stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
{
    static_assert(sizeof(element_type) == 1);

    if (buffer.empty())
    {
        co_return {io_status{io_status::kind::ok}, {}};
    }
    const auto deadline =
        timeout.count() > 0 ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point::max();
    const auto destination = std::span<element_type>{buffer.data(), buffer.size()};
    auto       remaining   = destination;
    while (!remaining.empty())
    {
        std::chrono::milliseconds remaining_timeout{0};
        if (timeout.count() > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                co_return {
                    io_status{io_status::kind::timeout}, destination.first(destination.size() - remaining.size())};
            }
            remaining_timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        }

        auto [status, received] = co_await stream.read_some(remaining, remaining_timeout, stop_token);
        remaining               = remaining.subspan(received.size());
        if (!status.is_ok())
        {
            co_return {status, destination.first(destination.size() - remaining.size())};
        }
    }
    co_return {io_status{io_status::kind::ok}, destination};
}

/**
 * Writes until all data is accepted or an error, cancellation or timeout occurs.
 * The stream must provide write_some(buffer, timeout, stop_token), returning a status and an unsent suffix.
 * Keep the stream and buffer alive until completion; do not run concurrent operations on the stream.
 * Accepted data follows the stream's contract: TLS may retain ciphertext after a flush timeout.
 * @param buffer Source buffer whose elements are one byte.
 * @param timeout Soft overall deadline for the operation; zero or negative waits indefinitely.
 * @param stop_token Requests cancellation, preserving the unsent portion of the buffer.
 * @return The status and the unsent suffix, even when an error-bearing operation accepts all remaining data.
 */
template<
    typename stream_type,
    concepts::const_buffer buffer_type,
    typename element_type = typename concepts::const_buffer_traits<buffer_type>::element_type>
auto write_all(
    stream_type&              stream,
    const buffer_type&        buffer,
    std::chrono::milliseconds timeout    = std::chrono::milliseconds{0},
    std::stop_token           stop_token = {}) -> coro::task<std::pair<io_status, std::span<element_type>>>
{
    static_assert(sizeof(element_type) == 1);

    if (buffer.empty())
    {
        co_return {io_status{io_status::kind::ok}, {}};
    }
    const auto deadline =
        timeout.count() > 0 ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point::max();
    auto remaining = std::span<element_type>{buffer.data(), buffer.size()};
    while (!remaining.empty())
    {
        std::chrono::milliseconds remaining_timeout{0};
        if (timeout.count() > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                co_return {io_status{io_status::kind::timeout}, remaining};
            }
            remaining_timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        }

        auto [status, unsent] = co_await stream.write_some(remaining, remaining_timeout, stop_token);
        remaining             = unsent;
        if (!status.is_ok())
        {
            co_return {status, remaining};
        }
    }
    co_return {io_status{io_status::kind::ok}, remaining};
}

} // namespace coro::net
