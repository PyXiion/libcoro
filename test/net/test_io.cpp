#include "catch_amalgamated.hpp"

#ifdef LIBCORO_FEATURE_NETWORKING

    #include <coro/coro.hpp>
    #include <coro/net/io.hpp>

    #include <algorithm>
    #include <array>
    #include <chrono>
    #include <span>
    #include <stop_token>
    #include <string>
    #include <string_view>
    #include <utility>

namespace
{
struct scripted_reader
{
    struct step
    {
        std::string_view     data;
        coro::net::io_status status;
    };

    std::array<step, 2> steps{
        {{"ab", {coro::net::io_status::kind::ok}}, {"cd", {coro::net::io_status::kind::native, 123}}}};
    std::size_t next{0};

    auto read_some(std::span<char> buffer, std::chrono::milliseconds, std::stop_token)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        const auto& current = steps.at(next++);
        const auto  size    = std::min(buffer.size(), current.data.size());
        std::copy_n(current.data.begin(), size, buffer.begin());
        co_return {current.status, buffer.first(size)};
    }
};

struct scripted_writer
{
    struct step
    {
        std::size_t          size;
        coro::net::io_status status;
    };

    std::array<step, 2> steps{{{2, {coro::net::io_status::kind::ok}}, {3, {coro::net::io_status::kind::native, 456}}}};
    std::size_t         next{0};
    std::string         consumed;

    auto write_some(std::span<const char> buffer, std::chrono::milliseconds, std::stop_token)
        -> coro::task<std::pair<coro::net::io_status, std::span<const char>>>
    {
        const auto& current = steps.at(next++);
        const auto  size    = std::min(buffer.size(), current.size);
        consumed.append(buffer.data(), size);
        co_return {current.status, buffer.subspan(size)};
    }
};
} // namespace

TEST_CASE("read_exact retains progress reported with an error", "[net_io]")
{
    const auto      size = GENERATE(std::size_t{4}, std::size_t{6});
    scripted_reader stream;
    std::string     buffer(size, '_');

    auto [status, received] = coro::sync_wait(coro::net::read_exact(stream, buffer));

    CHECK(status.type == coro::net::io_status::kind::native);
    CHECK(status.native_code == 123);
    CHECK(received.data() == buffer.data());
    CHECK(std::string_view{received.data(), received.size()} == "abcd");
    CHECK(buffer == "abcd" + std::string(size - 4, '_'));
}

TEST_CASE("write_all retains progress reported with an error", "[net_io]")
{
    const auto      input = GENERATE(std::string{"abcde"}, std::string{"abcdefg"});
    scripted_writer stream;

    auto [status, remaining] = coro::sync_wait(coro::net::write_all(stream, input));

    CHECK(status.type == coro::net::io_status::kind::native);
    CHECK(status.native_code == 456);
    CHECK(stream.consumed == "abcde");
    CHECK(remaining.data() == input.data() + stream.consumed.size());
    CHECK(std::string_view{remaining.data(), remaining.size()} == std::string_view{input}.substr(5));
}

TEST_CASE("read_exact keeps one deadline while TCP bytes arrive", "[net_io]")
{
    using namespace std::chrono_literals;

    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    coro::net::tcp::server server{scheduler, {"127.0.0.1", 0}};
    auto                   endpoint = coro::net::socket_address::make_uninitialised();
    auto [address, length]          = endpoint.native_mutable_data();
    REQUIRE(::getsockname(server.accept_socket().native_handle(), address, length) == 0);

    std::array<char, 16> buffer{};
    coro::net::io_status read_status{coro::net::io_status::kind::unknown};
    std::size_t          bytes_read{0};
    coro::event          writer_finished;

    auto make_reader_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tcp::client client{scheduler, endpoint};
        const auto             connected = co_await client.connect(2s);
        CHECK(connected == coro::net::connect_status::connected);
        if (connected != coro::net::connect_status::connected)
        {
            co_return;
        }

        auto [status, received] = co_await coro::net::read_exact(client, buffer, 200ms);
        read_status             = status;
        bytes_read              = received.size();

        // Keep the connection alive until the finite drip completes, even after a read timeout.
        co_await writer_finished;
    };
    auto make_writer_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        auto client = co_await server.accept(2s);
        CHECK(client.has_value());
        if (client.has_value())
        {
            const std::array<char, 1> byte{'x'};
            for (std::size_t i = 0; i < buffer.size(); ++i)
            {
                if (i != 0)
                {
                    co_await scheduler->yield_for(40ms);
                }
                auto [status, remaining] = co_await coro::net::write_all(*client, byte, 2s);
                CHECK(status.is_ok());
                CHECK(remaining.empty());
                if (!status.is_ok())
                {
                    break;
                }
            }
        }
        writer_finished.set();
    };

    coro::sync_wait(coro::when_all(make_writer_task(), make_reader_task()));

    CHECK(read_status.type == coro::net::io_status::kind::timeout);
    REQUIRE(bytes_read < buffer.size());
    CHECK(std::all_of(buffer.begin(), buffer.begin() + bytes_read, [](char value) -> bool { return value == 'x'; }));
    CHECK(std::all_of(buffer.begin() + bytes_read, buffer.end(), [](char value) -> bool { return value == '\0'; }));
}

#endif // LIBCORO_FEATURE_NETWORKING
