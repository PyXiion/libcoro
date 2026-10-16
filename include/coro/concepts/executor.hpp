#pragma once

#include "coro/concepts/awaitable.hpp"
#include "coro/fd.hpp"
#include "coro/task.hpp"

#ifdef LIBCORO_FEATURE_NETWORKING
    #include "coro/net/io_status.hpp"
    #include "coro/net/socket.hpp"
#endif // #ifdef LIBCORO_FEATURE_NETWORKING

#include <chrono>
#include <concepts>
#include <coroutine>
#include <span>
#include <stop_token>
#include <tuple>
#include <utility>

namespace coro::concepts
{

// clang-format off
template<typename executor_type>
concept executor = requires(executor_type e, std::coroutine_handle<> c)
{
    { e.schedule() } -> coro::concepts::awaiter;
    { e.spawn_detached(std::declval<coro::task<void>>()) } -> std::same_as<bool>;
    { e.spawn_joinable(std::declval<coro::task<void>>()) } -> std::same_as<coro::task<void>>;
    { e.yield() } -> coro::concepts::awaiter;
    { e.resume(c) } -> std::same_as<bool>;
    { e.size() } -> std::same_as<std::size_t>;
    { e.empty() } -> std::same_as<bool>;
    { e.shutdown() } -> std::same_as<void>;
};

#ifdef LIBCORO_FEATURE_NETWORKING
template<typename executor_type>
concept io_executor = executor<executor_type> and requires(
    executor_type e, net::socket& socket, const net::socket_address& endpoint,
    std::span<std::byte> buffer, std::span<const std::byte> const_buffer,
    std::chrono::milliseconds timeout, std::stop_token stop)
{
    { e.schedule_after(timeout) } -> std::same_as<coro::task<void>>;
    { e.yield_for(timeout) } -> std::same_as<coro::task<void>>;
    { e.read_some(socket, buffer, timeout, stop) } -> awaiter;
    { e.read_some(socket, buffer, timeout, stop).await_resume() } -> std::same_as<std::pair<net::io_status, std::size_t>>;
    { e.write_some(socket, const_buffer, timeout, stop) } -> awaiter;
    { e.write_some(socket, const_buffer, timeout, stop).await_resume() } -> std::same_as<std::pair<net::io_status, std::size_t>>;
    { e.accept(socket, timeout, stop) } -> awaiter;
    { e.accept(socket, timeout, stop).await_resume() } -> std::same_as<std::tuple<net::io_status, net::socket, net::socket_address>>;
    { e.connect(socket, endpoint, timeout, stop) } -> awaiter;
    { e.connect(socket, endpoint, timeout, stop).await_resume() } -> std::same_as<net::io_status>;
    { e.recv_from(socket, buffer, timeout, stop) } -> awaiter;
    { e.recv_from(socket, buffer, timeout, stop).await_resume() } -> std::same_as<std::tuple<net::io_status, std::size_t, net::socket_address>>;
    { e.send_to(socket, endpoint, const_buffer, timeout, stop) } -> awaiter;
    { e.send_to(socket, endpoint, const_buffer, timeout, stop).await_resume() } -> std::same_as<std::pair<net::io_status, std::size_t>>;
};
#endif // #ifdef LIBCORO_FEATURE_NETWORKING

// clang-format on

} // namespace coro::concepts
