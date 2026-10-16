#include "catch_amalgamated.hpp"

#include <coro/coro.hpp>
#include <coro/net/socket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stop_token>
#include <thread>

#include <sys/socket.h>

TEST_CASE("scheduler", "[scheduler]")
{
    std::cerr << "[scheduler]\n\n";
}

using namespace std::chrono_literals;

namespace
{
auto make_socket_pair() -> std::array<coro::net::socket, 2>
{
    std::array<int, 2> handles{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, handles.data()) == 0);
    auto sockets = std::array{coro::net::socket{handles[0]}, coro::net::socket{handles[1]}};
    REQUIRE(sockets[0].blocking(coro::net::socket::blocking_t::no));
    REQUIRE(sockets[1].blocking(coro::net::socket::blocking_t::no));
    return sockets;
}
} // namespace

TEST_CASE("scheduler schedule single task", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_task = [](std::unique_ptr<coro::scheduler>& s) -> coro::task<uint64_t>
    {
        co_await s->schedule();
        co_return 42;
    };

    auto value = coro::sync_wait(make_task(s));
    REQUIRE(value == 42);
    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler submit mutiple tasks", "[scheduler]")
{
    constexpr std::size_t         n = 1000;
    std::atomic<uint64_t>         counter{0};
    std::vector<coro::task<void>> tasks{};
    tasks.reserve(n);
    auto s = coro::scheduler::make_unique();

    auto make_task = [](std::unique_ptr<coro::scheduler>& s, std::atomic<uint64_t>& counter) -> coro::task<void>
    {
        co_await s->schedule();
        counter++;
        co_return;
    };
    for (std::size_t i = 0; i < n; ++i)
    {
        tasks.emplace_back(make_task(s, counter));
    }

    coro::sync_wait(coro::when_all(std::move(tasks)));

    REQUIRE(counter == n);

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler task with multiple events", "[scheduler]")
{
    std::atomic<uint64_t> counter{0};
    auto                  s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    coro::event e1;
    coro::event e2;
    coro::event e3;

    auto make_wait_task = [](std::unique_ptr<coro::scheduler>& s,
                             std::atomic<uint64_t>&            counter,
                             coro::event&                      e1,
                             coro::event&                      e2,
                             coro::event&                      e3) -> coro::task<void>
    {
        co_await s->schedule();
        co_await e1;
        counter++;
        co_await e2;
        counter++;
        co_await e3;
        counter++;
        co_return;
    };

    auto make_set_task = [](std::unique_ptr<coro::scheduler>& s, coro::event& e) -> coro::task<void>
    {
        co_await s->schedule();
        e.set();
    };

    coro::sync_wait(coro::when_all(
        make_wait_task(s, counter, e1, e2, e3), make_set_task(s, e1), make_set_task(s, e2), make_set_task(s, e3)));

    REQUIRE(counter == 3);

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler task with read completion", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_read_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
        REQUIRE(buffer[0] == std::byte{42});
    };

    auto make_write_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        const auto buffer    = std::array{std::byte{42}};
        auto [status, bytes] = co_await s->write_some(sockets[1], buffer);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
    };

    coro::sync_wait(coro::when_all(make_read_task(), make_write_task()));
    s->shutdown();
    REQUIRE(s->empty());
}

TEST_CASE("scheduler task with read completion before timeout", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_read_task = [&]() -> coro::task<void>
    {
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer, 5s);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
        REQUIRE(buffer[0] == std::byte{42});
    };

    auto make_write_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        const auto buffer    = std::array{std::byte{42}};
        auto [status, bytes] = co_await s->write_some(sockets[1], buffer);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
    };

    auto read_task = make_read_task();
    read_task.resume();
    REQUIRE_FALSE(read_task.is_ready());
    coro::sync_wait(make_write_task());
    s->shutdown();
    REQUIRE(s->empty());
    REQUIRE(read_task.is_ready());
    coro::sync_wait(read_task);
}

TEST_CASE("scheduler task with read timeout", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer, 10ms);
        REQUIRE(status.is_timeout());
        REQUIRE(bytes == 0);
    };

    coro::sync_wait(make_task());
    s->shutdown();
    REQUIRE(s->empty());
}

TEST_CASE("scheduler pending read cancellation", "[scheduler]")
{
    auto sockets  = make_socket_pair();
    auto strategy = GENERATE(
        coro::scheduler::execution_strategy_t::process_tasks_inline,
        coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool);
    auto s = coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy    = coro::scheduler::thread_strategy_t::manual,
            .pool               = coro::thread_pool::options{.thread_count = 1},
            .execution_strategy = strategy});
    std::stop_source stop_source;

    auto make_read_task = [&]() -> coro::task<void>
    {
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer, 5s, stop_source.get_token());
        REQUIRE(status.type == coro::net::io_status::kind::cancelled);
        REQUIRE(bytes == 0);
    };
    auto make_cancel_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        REQUIRE(stop_source.request_stop());
    };

    auto read_task = make_read_task();
    read_task.resume();
    REQUIRE_FALSE(read_task.is_ready());
    REQUIRE(s->size() == 1);
    REQUIRE_FALSE(s->empty());

    auto cancel_task = make_cancel_task();
    cancel_task.resume();
    while (s->process_events(100ms) > 0) {}
    s->shutdown();
    REQUIRE(s->empty());
    REQUIRE(read_task.is_ready());
    REQUIRE(cancel_task.is_ready());
    coro::sync_wait(read_task);
    coro::sync_wait(cancel_task);
}

TEST_CASE("scheduler pre-cancelled read preserves later data", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    std::stop_source stop_source;
    REQUIRE(stop_source.request_stop());

    auto make_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        std::array<std::byte, 1> buffer{};
        auto [cancel_status, cancel_bytes] = co_await s->read_some(sockets[0], buffer, 5s, stop_source.get_token());
        REQUIRE(cancel_status.type == coro::net::io_status::kind::cancelled);
        REQUIRE(cancel_bytes == 0);

        const auto value             = std::array{std::byte{42}};
        auto [write_status, written] = co_await s->write_some(sockets[1], value);
        REQUIRE(write_status.is_ok());
        REQUIRE(written == 1);

        auto [read_status, bytes] = co_await s->read_some(sockets[0], buffer, 5s);
        REQUIRE(read_status.is_ok());
        REQUIRE(bytes == 1);
        REQUIRE(buffer[0] == value[0]);
    };

    coro::sync_wait(make_task());
    s->shutdown();
    REQUIRE(s->empty());
}

TEST_CASE("scheduler separate thread resume", "[scheduler]")
{
    auto s1 =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    auto s2 =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    coro::event e{};

    auto make_s1_task = [](std::unique_ptr<coro::scheduler>& s1, coro::event& e) -> coro::task<void>
    {
        co_await s1->schedule();
        auto tid = std::this_thread::get_id();
        co_await e;

        // This coroutine will hop to the other scheduler's single thread upon resuming.
        REQUIRE_FALSE(tid == std::this_thread::get_id());

        co_return;
    };

    auto make_s2_task = [](std::unique_ptr<coro::scheduler>& s2, coro::event& e) -> coro::task<void>
    {
        co_await s2->schedule();
        // Wait a bit to be sure the wait on 'e' in the other scheduler is done first.
        std::this_thread::sleep_for(10ms);
        e.set();
        co_return;
    };

    coro::sync_wait(coro::when_all(make_s1_task(s1, e), make_s2_task(s2, e)));

    s1->shutdown();
    REQUIRE(s1->empty());
    s2->shutdown();
    REQUIRE(s2->empty());
}

TEST_CASE("scheduler separate thread resume spawned thread", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_task = [](std::unique_ptr<coro::scheduler>& s) -> coro::task<void>
    {
        co_await s->schedule();
        coro::event e{};

        auto tid = std::this_thread::get_id();

        // Normally this thread is probably already running for real world use cases, but in general
        // the 3rd party function api will be set, they should have "user data" void* or ability
        // to capture variables via lambdas for on complete callbacks, here we mimic an on complete
        // callback by capturing the hande.
        std::thread third_party_thread(
            [&e, &s]() -> void
            {
                // mimic some expensive computation
                // Resume the coroutine back onto the scheduler, not this background thread.
                e.set(s);
            });
        third_party_thread.detach();

        // Wait on the handle until the 3rd party service is completed.
        co_await e;
        REQUIRE(tid == std::this_thread::get_id());
    };

    coro::sync_wait(make_task(s));

    s->shutdown();
    REQUIRE(s->empty());
}

TEST_CASE("scheduler separate thread resume with return", "[scheduler]")
{
    constexpr uint64_t expected_value{1337};
    auto               s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    coro::event           start_service{};
    coro::event           service_done{};
    std::atomic<uint64_t> output;

    std::thread service{
        [&]() -> void
        {
            while (!start_service.is_set())
            {
                std::this_thread::sleep_for(1ms);
            }

            output = expected_value;
            service_done.set(s);
        }};

    auto make_task = [](std::unique_ptr<coro::scheduler>& s,
                        coro::event&                      start_service,
                        coro::event&                      service_done,
                        std::atomic<uint64_t>&            output) -> coro::task<void>
    {
        auto third_party_service = [](coro::event&           start_service,
                                      coro::event&           service_done,
                                      int                    multiplier,
                                      std::atomic<uint64_t>& output) -> coro::task<uint64_t>
        {
            start_service.set();
            co_await service_done;
            co_return output* multiplier;
        };

        co_await s->schedule();

        int      multiplier{5};
        uint64_t value = co_await third_party_service(start_service, service_done, multiplier, output);
        REQUIRE(value == (expected_value * multiplier));
    };

    coro::sync_wait(make_task(s, start_service, service_done, output));

    service.join();
    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler with basic task", "[scheduler]")
{
    constexpr std::size_t expected_value{5};
    auto                  s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto func = [](std::unique_ptr<coro::scheduler>& s) -> coro::task<int>
    {
        auto add_data = [](std::unique_ptr<coro::scheduler>& s, uint64_t val) -> coro::task<int>
        {
            co_await s->schedule();
            co_return val;
        };

        co_await s->schedule();

        auto output_tasks =
            co_await coro::when_all(add_data(s, 1), add_data(s, 1), add_data(s, 1), add_data(s, 1), add_data(s, 1));

        int counter{0};
        std::apply([&counter](auto&&... tasks) -> void { ((counter += tasks.return_value()), ...); }, output_tasks);

        co_return counter;
    };

    auto counter = coro::sync_wait(func(s));

    REQUIRE(counter == expected_value);

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler scheduler_after", "[scheduler]")
{
    constexpr std::chrono::milliseconds wait_for{50};
    std::atomic<uint64_t>               counter{0};
    std::thread::id                     tid;

    auto func = [](coro::scheduler&          s,
                   std::chrono::milliseconds amount,
                   std::atomic<uint64_t>&    counter,
                   std::thread::id&          tid) -> coro::task<void>
    {
        co_await s.schedule_after(amount);
        ++counter;
        // Make sure schedule after context switches into the worker thread.
        REQUIRE(tid == std::this_thread::get_id());
        co_return;
    };

    {
        auto s     = coro::scheduler::make_unique(coro::scheduler::options{
                .pool = coro::thread_pool::options{
                    .thread_count = 1, .on_thread_start_functor = [&](std::size_t) { tid = std::this_thread::get_id(); }}});
        auto start = std::chrono::steady_clock::now();
        coro::sync_wait(func(*s, 0ms, counter, tid));
        auto stop     = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

        REQUIRE(counter == 1);
        REQUIRE(duration < wait_for);
        std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
        s->shutdown();
        std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
        REQUIRE(s->empty());
    }

    {
        auto s = coro::scheduler::make_unique(coro::scheduler::options{
            .pool = coro::thread_pool::options{
                .thread_count = 1, .on_thread_start_functor = [&](std::size_t) { tid = std::this_thread::get_id(); }}});

        auto start = std::chrono::steady_clock::now();
        coro::sync_wait(func(*s, wait_for, counter, tid));
        auto stop     = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

        REQUIRE(counter == 2);
        REQUIRE(duration >= wait_for);
        std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
        s->shutdown();
        std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
        REQUIRE(s->empty());
    }
}

TEST_CASE("scheduler schedule_at", "[scheduler]")
{
    // Because schedule_at() will take its own time internally the wait_for might be off by a bit.
    constexpr std::chrono::milliseconds epsilon{3};
    constexpr std::chrono::milliseconds wait_for{50};
    std::atomic<uint64_t>               counter{0};
    std::thread::id                     tid;

    auto s = coro::scheduler::make_unique(coro::scheduler::options{
        .pool = coro::thread_pool::options{
            .thread_count = 1, .on_thread_start_functor = [&](std::size_t) { tid = std::this_thread::get_id(); }}});

    auto func = [](std::unique_ptr<coro::scheduler>&     s,
                   std::atomic<uint64_t>&                counter,
                   std::chrono::steady_clock::time_point time,
                   std::thread::id&                      tid) -> coro::task<void>
    {
        co_await s->schedule_at(time);
        ++counter;
        REQUIRE(tid == std::this_thread::get_id());
        co_return;
    };

    {
        auto start = std::chrono::steady_clock::now();
        coro::sync_wait(func(s, counter, std::chrono::steady_clock::now() + wait_for, tid));
        auto stop     = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

        REQUIRE(counter == 1);
        REQUIRE(duration >= (wait_for - epsilon));
    }

    {
        auto start = std::chrono::steady_clock::now();
        coro::sync_wait(func(s, counter, std::chrono::steady_clock::now(), tid));
        auto stop     = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

        REQUIRE(counter == 2);
        REQUIRE(duration <= 10ms); // Just verify its less than the wait_for time period.
    }

    {
        auto start = std::chrono::steady_clock::now();
        coro::sync_wait(func(s, counter, std::chrono::steady_clock::now() - 1s, tid));
        auto stop     = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

        REQUIRE(counter == 3);
        REQUIRE(duration <= 10ms);
    }
}

TEST_CASE("scheduler yield", "[scheduler]")
{
    std::thread::id tid;
    auto            s = coro::scheduler::make_unique(coro::scheduler::options{
                   .pool = coro::thread_pool::options{
                       .thread_count = 1, .on_thread_start_functor = [&](std::size_t) { tid = std::this_thread::get_id(); }}});

    auto func = [](std::unique_ptr<coro::scheduler>& s, std::thread::id& tid) -> coro::task<void>
    {
        REQUIRE(tid != std::this_thread::get_id());
        co_await s->schedule();
        REQUIRE(tid == std::this_thread::get_id());
        co_await s->yield(); // this is really a thread pool function but /shrug
        REQUIRE(tid == std::this_thread::get_id());
        co_return;
    };

    coro::sync_wait(func(s, tid));

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler yield_for", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    const std::chrono::milliseconds wait_for{50};

    auto make_task = [](std::unique_ptr<coro::scheduler>& s,
                        std::chrono::milliseconds         wait_for) -> coro::task<std::chrono::milliseconds>
    {
        co_await s->schedule();
        auto start = std::chrono::steady_clock::now();
        co_await s->yield_for(wait_for);
        co_return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    };

    auto duration = coro::sync_wait(make_task(s, wait_for));
    REQUIRE(duration >= wait_for);

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler yield_until", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    // Because yield_until() takes its own time internally the wait_for might be off by a bit.
    const std::chrono::milliseconds epsilon{3};
    const std::chrono::milliseconds wait_for{50};

    auto make_task = [](std::unique_ptr<coro::scheduler>& s,
                        std::chrono::milliseconds         wait_for) -> coro::task<std::chrono::milliseconds>
    {
        co_await s->schedule();
        auto start = std::chrono::steady_clock::now();
        co_await s->yield_until(start + wait_for);
        co_return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    };

    auto duration = coro::sync_wait(make_task(s, wait_for));
    REQUIRE(duration >= (wait_for - epsilon));

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler multipler event waiters", "[scheduler]")
{
    const constexpr std::size_t total{10};
    coro::event                 e{};
    auto                        s = coro::scheduler::make_unique();

    auto spawn = [](std::unique_ptr<coro::scheduler>& s, coro::event& e) -> coro::task<void>
    {
        auto func = [](coro::event& e) -> coro::task<uint64_t>
        {
            co_await e;
            co_return 1;
        };

        co_await s->schedule();
        std::vector<coro::task<uint64_t>> tasks;
        for (size_t i = 0; i < total; ++i)
        {
            tasks.emplace_back(func(e));
        }

        auto results = co_await coro::when_all(std::move(tasks));

        uint64_t counter{0};
        for (const auto& task : results)
        {
            counter += task.return_value();
        }
        REQUIRE(counter == total);
    };

    auto release = [](std::unique_ptr<coro::scheduler>& s, coro::event& e) -> coro::task<void>
    {
        co_await s->schedule_after(10ms);
        e.set(s);
    };

    coro::sync_wait(coro::when_all(spawn(s, e), release(s, e)));

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler self generating coroutine (stack overflow check)", "[scheduler]")
{
    const constexpr std::size_t total{1'000'000};
    uint64_t                    counter{0};
    auto                        s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    std::vector<coro::task<void>> tasks;
    tasks.reserve(total);

    auto func = [](std::unique_ptr<coro::scheduler>& s, uint64_t& counter, auto f, std::vector<coro::task<void>>& tasks)
        -> coro::task<void>
    {
        co_await s->schedule();
        ++counter;

        if (counter % total == 0)
        {
            co_return;
        }

        // co_await f(f) _will_ stack overflow since each coroutine links to its parent, by storing
        // each new invocation into the vector they are not linked, but we can make sure the scheduler
        // doesn't choke on this many tasks being scheduled.
        tasks.emplace_back(f(s, counter, f, tasks));
        tasks.back().resume();
        co_return;
    };

    coro::sync_wait(func(s, counter, func, tasks));

    while (tasks.size() < total - 1)
    {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(tasks.size() == total - 1);

    std::cerr << "scheduler.size() before shutdown = " << s->size() << "\n";
    s->shutdown();
    std::cerr << "scheduler.size() after shutdown = " << s->size() << "\n";
    REQUIRE(s->empty());
}

TEST_CASE("scheduler manual process events thread pool", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s       = coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool            = coro::thread_pool::options{.thread_count = 1}});

    auto make_read_task = [&]() -> coro::task<void>
    {
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer, 5s);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
        REQUIRE(buffer[0] == std::byte{42});
    };
    auto make_write_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        const auto buffer    = std::array{std::byte{42}};
        auto [status, bytes] = co_await s->write_some(sockets[1], buffer);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
    };

    auto read_task = make_read_task();
    read_task.resume();
    REQUIRE_FALSE(read_task.is_ready());
    REQUIRE(s->size() == 1);
    REQUIRE_FALSE(s->empty());

    auto write_task = make_write_task();
    write_task.resume();
    while (s->process_events(100ms) > 0) {}
    s->shutdown();
    REQUIRE(s->empty());
    REQUIRE(read_task.is_ready());
    REQUIRE(write_task.is_ready());
    coro::sync_wait(read_task);
    coro::sync_wait(write_task);
}

TEST_CASE("scheduler manual process events inline", "[scheduler]")
{
    auto sockets = make_socket_pair();
    auto s = coro::scheduler::make_unique(coro::scheduler::options{
        .thread_strategy    = coro::scheduler::thread_strategy_t::manual,
        .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_inline});

    auto make_read_task = [&]() -> coro::task<void>
    {
        std::array<std::byte, 1> buffer{};
        auto [status, bytes] = co_await s->read_some(sockets[0], buffer, 5s);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
        REQUIRE(buffer[0] == std::byte{42});
    };
    auto make_write_task = [&]() -> coro::task<void>
    {
        co_await s->schedule();
        const auto buffer    = std::array{std::byte{42}};
        auto [status, bytes] = co_await s->write_some(sockets[1], buffer);
        REQUIRE(status.is_ok());
        REQUIRE(bytes == 1);
    };

    auto read_task = make_read_task();
    read_task.resume();
    REQUIRE_FALSE(read_task.is_ready());
    REQUIRE(s->size() == 1);
    REQUIRE_FALSE(s->empty());

    auto write_task = make_write_task();
    write_task.resume();
    REQUIRE(s->size() == 2);
    while (s->process_events(100ms) > 0) {}
    s->shutdown();
    REQUIRE(s->empty());
    REQUIRE(read_task.is_ready());
    REQUIRE(write_task.is_ready());
    coro::sync_wait(read_task);
    coro::sync_wait(write_task);
}

TEST_CASE("scheduler task throws", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto func = [](std::unique_ptr<coro::scheduler>& s) -> coro::task<uint64_t>
    {
        co_await s->schedule();
        throw std::runtime_error{"I always throw."};
        co_return 42;
    };

    REQUIRE_THROWS(coro::sync_wait(func(s)));
}

TEST_CASE("scheduler task throws after resume", "[scheduler]")
{
    auto s =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto make_thrower = [](std::unique_ptr<coro::scheduler>& s) -> coro::task<bool>
    {
        co_await s->schedule();
        std::cerr << "Throwing task is doing some work...\n";
        co_await s->yield();
        throw std::runtime_error{"I always throw."};
        co_return true;
    };

    REQUIRE_THROWS(coro::sync_wait(make_thrower(s)));
}

TEST_CASE("coro::scheduler::spawn", "[scheduler]")
{
    // issue-287

    const int ITERATIONS = 200000;

    std::atomic<uint32_t> g_count = 0;
    auto                  scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto task = [](std::atomic<uint32_t>& count) -> coro::task<void>
    {
        count++;
        co_return;
    };

    for (int i = 0; i < ITERATIONS; ++i)
    {
        REQUIRE(scheduler->spawn_detached(task(g_count)));
    }

    scheduler->shutdown();

    std::cerr << "g_count = \t" << g_count.load() << std::endl;
    REQUIRE(g_count.load() == ITERATIONS);
}

TEST_CASE("scheduler::schedule(task)", "[scheduler]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    uint64_t        counter{0};
    std::thread::id coroutine_tid;

    auto make_task = [](uint64_t value, std::thread::id& coroutine_id) -> coro::task<uint64_t>
    {
        coroutine_id = std::this_thread::get_id();
        co_return value;
    };

    auto main_tid = std::this_thread::get_id();

    counter += coro::sync_wait(scheduler->schedule(make_task(53, coroutine_tid)));

    REQUIRE(counter == 53);
    REQUIRE(main_tid != coroutine_tid);
}

TEST_CASE("scheduler shutdown with background yielding task", "[scheduler]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});

    auto background_task = [](coro::scheduler* scheduler) -> coro::task<void> { co_await scheduler->yield_for(20ms); };
    scheduler->spawn_detached(background_task(scheduler.get()));

    // std::this_thread::sleep_for(21ms);  //! Required to avoid hang at `yield_for`
    scheduler->shutdown();
}

TEST_CASE("scheduler schedule with <= 0ms timeout", "[scheduler]")
{
    auto scheduler = coro::scheduler::make_unique(
        coro::scheduler::options{.execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_inline});

    auto make_task = [](int i) -> coro::task<int> { co_return i; };

    auto result = coro::sync_wait(scheduler->schedule(std::stop_source{}, make_task(1), std::chrono::milliseconds{0}));
    REQUIRE(result == 1);
    result = coro::sync_wait(scheduler->schedule(std::stop_source{}, make_task(2), std::chrono::milliseconds{-1}));
    REQUIRE(result == 2);
}

TEST_CASE("~scheduler", "[scheduler]")
{
    std::cerr << "[~scheduler]\n\n";
}
