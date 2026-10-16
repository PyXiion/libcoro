#include "catch_amalgamated.hpp"
#include "catch_extensions.hpp"
#include "catch_net_extensions.hpp"
#include "coro/net/socket_address.hpp"

#ifdef LIBCORO_FEATURE_NETWORKING
    #ifdef LIBCORO_FEATURE_TLS

        #include <coro/coro.hpp>

        #include <stop_token>

TEST_CASE("tls_server hello world server", "[tls_server]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    const std::string client_msg = "Hello world from TLS client!";
    const std::string server_msg = "Hello world from TLS server!!";
    const auto        endpoint   = coro::net::socket_address{"127.0.0.1", 8080};
    coro::net::tls::server server{
        scheduler,
        std::make_shared<coro::net::tls::context>(
            "cert.pem", coro::net::tls::tls_file_type::pem, "key.pem", coro::net::tls::tls_file_type::pem),
        endpoint};

    auto make_client_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tls::client client{
            scheduler, std::make_shared<coro::net::tls::context>(coro::net::tls::verify_peer_t::no), endpoint};

        auto cstatus = co_await client.connect(std::chrono::seconds{5});
        REQUIRE(cstatus == coro::net::tls::connection_status::connected);

        auto [sstatus, remaining] = co_await client.write_all(client_msg, std::chrono::seconds{5});
        REQUIRE(sstatus.is_ok());
        REQUIRE(remaining.empty());

        std::string response(server_msg.size(), '\0');
        auto [rstatus, rspan] = co_await client.read_exact(response, std::chrono::seconds{5});
        REQUIRE(rstatus.is_ok());
        REQUIRE(rspan.size() == server_msg.size());
        REQUIRE(response == server_msg);
        co_await client.shutdown(std::chrono::seconds{5});
    };

    auto make_server_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        auto client = co_await server.accept(std::chrono::seconds{5}, std::chrono::seconds{5});
        REQUIRE(client.has_value());
        REQUIRE(client->socket().is_ok());

        std::string buffer(client_msg.size(), '\0');
        auto [rstatus, rspan] = co_await client->read_exact(buffer, std::chrono::seconds{5});
        REQUIRE(rstatus.is_ok());
        REQUIRE(rspan.size() == client_msg.size());
        REQUIRE(buffer == client_msg);

        auto [sstatus, remaining] = co_await client->write_all(server_msg, std::chrono::seconds{5});
        REQUIRE(sstatus.is_ok());
        REQUIRE(remaining.empty());
        co_await client->shutdown(std::chrono::seconds{5});
    };

    coro::sync_wait(coro::when_all(make_server_task(), make_client_task()));
}

TEST_CASE("tls_server read timeout preserves connection", "[tls_server]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    const auto             endpoint = coro::net::socket_address{"127.0.0.1", 8080};
    coro::net::tls::server server{
        scheduler,
        std::make_shared<coro::net::tls::context>(
            "cert.pem", coro::net::tls::tls_file_type::pem, "key.pem", coro::net::tls::tls_file_type::pem),
        endpoint};
    const std::string message = "still connected after timeout";

    auto make_client_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tls::client client{
            scheduler, std::make_shared<coro::net::tls::context>(coro::net::tls::verify_peer_t::no), endpoint};
        auto cstatus = co_await client.connect(std::chrono::seconds{5});
        REQUIRE(cstatus == coro::net::tls::connection_status::connected);

        std::string buffer(message.size(), '\0');
        auto [timeout_status, empty] = co_await client.read_some(buffer, std::chrono::milliseconds{25});
        REQUIRE(timeout_status.is_timeout());
        REQUIRE(empty.empty());

        auto [write_status, remaining] = co_await client.write_all(message, std::chrono::seconds{5});
        REQUIRE(write_status.is_ok());
        REQUIRE(remaining.empty());
        auto [read_status, received] = co_await client.read_exact(buffer, std::chrono::seconds{5});
        REQUIRE(read_status.is_ok());
        REQUIRE(received.size() == message.size());
        REQUIRE(buffer == message);
        co_await client.shutdown(std::chrono::seconds{5});
    };

    auto make_server_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        auto client = co_await server.accept(std::chrono::seconds{5}, std::chrono::seconds{5});
        REQUIRE(client.has_value());

        // Send nothing until the client has timed out and sends its request.
        std::string buffer(message.size(), '\0');
        auto [read_status, received] = co_await client->read_exact(buffer, std::chrono::seconds{5});
        REQUIRE(read_status.is_ok());
        REQUIRE(received.size() == message.size());
        REQUIRE(buffer == message);
        auto [write_status, remaining] = co_await client->write_all(buffer, std::chrono::seconds{5});
        REQUIRE(write_status.is_ok());
        REQUIRE(remaining.empty());
        co_await client->shutdown(std::chrono::seconds{5});
    };

    coro::sync_wait(coro::when_all(make_server_task(), make_client_task()));
}

TEST_CASE("tls_server shutdown closes peer read", "[tls_server]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    const auto             endpoint = coro::net::socket_address{"127.0.0.1", 8080};
    coro::net::tls::server server{
        scheduler,
        std::make_shared<coro::net::tls::context>(
            "cert.pem", coro::net::tls::tls_file_type::pem, "key.pem", coro::net::tls::tls_file_type::pem),
        endpoint};

    auto make_client_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tls::client client{
            scheduler, std::make_shared<coro::net::tls::context>(coro::net::tls::verify_peer_t::no), endpoint};
        auto cstatus = co_await client.connect(std::chrono::seconds{5});
        REQUIRE(cstatus == coro::net::tls::connection_status::connected);

        std::string buffer(1, '\0');
        auto [status, received] = co_await client.read_some(buffer, std::chrono::seconds{5});
        REQUIRE(status.is_closed());
        REQUIRE(received.empty());
        co_await client.shutdown(std::chrono::seconds{5});
    };

    auto make_server_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        auto client = co_await server.accept(std::chrono::seconds{5}, std::chrono::seconds{5});
        REQUIRE(client.has_value());
        co_await client->shutdown(std::chrono::seconds{5});
    };

    coro::sync_wait(coro::when_all(make_server_task(), make_client_task()));
}

TEST_CASE("tls_server cancels pending read", "[tls_server]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    const auto             endpoint = coro::net::socket_address{"127.0.0.1", 8080};
    coro::net::tls::server server{
        scheduler,
        std::make_shared<coro::net::tls::context>(
            "cert.pem", coro::net::tls::tls_file_type::pem, "key.pem", coro::net::tls::tls_file_type::pem),
        endpoint};
    coro::event read_finished;

    auto make_client_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        coro::net::tls::client client{
            scheduler, std::make_shared<coro::net::tls::context>(coro::net::tls::verify_peer_t::no), endpoint};
        auto cstatus = co_await client.connect(std::chrono::seconds{5});
        REQUIRE(cstatus == coro::net::tls::connection_status::connected);

        // The peer stays idle until read_finished, so only the stop request can complete this read early.
        std::stop_source stop_source;
        auto             reader = [&]() -> coro::task<coro::net::io_status>
        {
            std::string buffer(1, '\0');
            auto [status, received] =
                co_await client.read_some(buffer, std::chrono::seconds{5}, stop_source.get_token());
            REQUIRE_THREAD_SAFE(received.empty());
            co_return status;
        };
        auto canceller = [&]() -> coro::task<void>
        {
            co_await scheduler->yield_for(std::chrono::milliseconds{50});
            stop_source.request_stop();
        };
        auto [read_task, cancel_task] = co_await coro::when_all(reader(), canceller());
        REQUIRE(read_task.return_value().type == coro::net::io_status::kind::cancelled);

        read_finished.set();
        co_await client.shutdown(std::chrono::seconds{5});
    };

    auto make_server_task = [&]() -> coro::task<void>
    {
        co_await scheduler->schedule();
        auto client = co_await server.accept(std::chrono::seconds{5}, std::chrono::seconds{5});
        REQUIRE(client.has_value());
        co_await read_finished;
        co_await client->shutdown(std::chrono::seconds{5});
    };

    coro::sync_wait(coro::when_all(make_server_task(), make_client_task()));
}

    #endif // LIBCORO_FEATURE_TLS
#endif     // LIBCORO_FEATURE_NETWORKING
