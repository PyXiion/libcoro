#include "catch_amalgamated.hpp"

#ifdef LIBCORO_FEATURE_NETWORKING

    #include <coro/coro.hpp>

    #include <coroutine>
    #include <future>
    #include <memory>
    #include <thread>
    #include <utility>
    #include <vector>

namespace
{
/// A conforming executor whose resume() runs the handle inline; schedule() still moves onto its pool.
class inline_resume_executor
{
public:
    explicit inline_resume_executor(std::unique_ptr<coro::thread_pool> pool) : m_pool(std::move(pool)) {}

    auto schedule() { return m_pool->schedule(); }
    auto spawn_detached(coro::task<void>&& task) -> bool { return m_pool->spawn_detached(std::move(task)); }
    auto spawn_joinable(coro::task<void>&& task) -> coro::task<void> { return m_pool->spawn_joinable(std::move(task)); }
    auto yield() { return m_pool->yield(); }
    auto resume(std::coroutine_handle<> handle) -> bool
    {
        handle.resume();
        return true;
    }
    auto size() -> std::size_t { return m_pool->size(); }
    auto empty() -> bool { return m_pool->empty(); }
    auto shutdown() -> void { m_pool->shutdown(); }

private:
    std::unique_ptr<coro::thread_pool> m_pool;
};
} // namespace

TEST_CASE("dns lookup owns its hostname and returns addresses that outlive the resolver", "[dns]")
{
    const auto [hostname, domain] =
        GENERATE(std::pair{"127.0.0.1", coro::net::domain_t::ipv4}, std::pair{"::1", coro::net::domain_t::ipv6});
    coro::net::dns::result result;
    {
        auto scheduler = coro::scheduler::make_unique(
            coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
        coro::net::dns::resolver resolver{scheduler};

        auto lookup = resolver.host_by_name(coro::net::hostname{hostname});
        result      = coro::sync_wait(scheduler->schedule(std::move(lookup)));
        scheduler->shutdown();
    }

    REQUIRE(result.is_ok());
    CHECK_FALSE(result.is_error());
    CHECK(static_cast<bool>(result));
    CHECK(result.status() == coro::net::dns::status::complete);
    CHECK(
        result.ip_addresses() ==
        std::vector<coro::net::ip_address>{coro::net::ip_address::from_string(hostname, domain)});
}

TEST_CASE("dns lookup reports failure without addresses", "[dns]")
{
    coro::net::dns::result result;
    {
        auto executor = coro::thread_pool::make_unique(coro::thread_pool::options{.thread_count = 1});
        coro::net::dns::resolver resolver{executor};

        result = coro::sync_wait(executor->schedule(resolver.host_by_name(coro::net::hostname{"service.onion"})));
        executor->shutdown();
    }

    REQUIRE(result.is_error());
    CHECK_FALSE(result.is_ok());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.status() == coro::net::dns::status::error);
    CHECK(result.ip_addresses().empty());
}

TEST_CASE("dns resolvers share library lifetime across executors and concurrent lookups", "[dns]")
{
    auto scheduler =
        coro::scheduler::make_unique(coro::scheduler::options{.pool = coro::thread_pool::options{.thread_count = 1}});
    coro::net::dns::resolver                                      scheduler_resolver{scheduler};
    std::vector<std::future<std::vector<coro::net::dns::result>>> workers;

    for (std::size_t worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back(
            std::async(
                std::launch::async,
                []() -> std::vector<coro::net::dns::result>
                {
                    auto executor = coro::thread_pool::make_unique(coro::thread_pool::options{.thread_count = 2});
                    std::vector<coro::net::dns::result> results;
                    for (std::size_t iteration = 0; iteration < 4; ++iteration)
                    {
                        coro::net::dns::resolver resolver{executor};
                        auto [success, failure] = coro::sync_wait(
                            coro::when_all(
                                executor->schedule(resolver.host_by_name(coro::net::hostname{"127.0.0.1"})),
                                executor->schedule(resolver.host_by_name(coro::net::hostname{"service.onion"}))));
                        results.emplace_back(std::move(success.return_value()));
                        results.emplace_back(std::move(failure.return_value()));
                    }
                    executor->shutdown();
                    return results;
                }));
    }

    for (auto& worker : workers)
    {
        auto results = worker.get();
        REQUIRE(results.size() == 8);
        for (std::size_t index = 0; index < results.size(); index += 2)
        {
            REQUIRE(results[index].is_ok());
            CHECK(
                results[index].ip_addresses() ==
                std::vector<coro::net::ip_address>{coro::net::ip_address::from_string("127.0.0.1")});
            CHECK(results[index + 1].is_error());
            CHECK(results[index + 1].ip_addresses().empty());
        }
    }

    auto result = coro::sync_wait(scheduler->schedule(scheduler_resolver.host_by_name(coro::net::hostname{"::1"})));
    scheduler->shutdown();
    REQUIRE(result.is_ok());
    CHECK(
        result.ip_addresses() ==
        std::vector<coro::net::ip_address>{coro::net::ip_address::from_string("::1", coro::net::domain_t::ipv6)});
}

TEST_CASE("dns resolver constructor failure does not prevent subsequent use", "[dns]")
{
    std::unique_ptr<coro::thread_pool> executor;
    REQUIRE_THROWS_AS(coro::net::dns::resolver{executor}, std::runtime_error);

    executor = coro::thread_pool::make_unique(coro::thread_pool::options{.thread_count = 1});
    coro::net::dns::resolver resolver{executor};
    auto result = coro::sync_wait(executor->schedule(resolver.host_by_name(coro::net::hostname{"127.0.0.1"})));
    executor->shutdown();
    REQUIRE(result.is_ok());
}

TEST_CASE("dns lookup completed on the c-ares thread may destroy its resolver", "[dns]")
{
    std::thread::id pool_thread;
    auto            executor = std::make_unique<inline_resume_executor>(coro::thread_pool::make_unique(
        coro::thread_pool::options{
            .thread_count            = 1,
            .on_thread_start_functor = [&](std::size_t) { pool_thread = std::this_thread::get_id(); }}));
    auto            resolver = std::make_unique<coro::net::dns::resolver<inline_resume_executor>>(executor);

    auto lookup = [&]() -> coro::task<std::thread::id>
    {
        co_await executor->schedule();
        // c-ares cannot answer this name locally, so whether the query succeeds, fails or times out its callback runs
        // on the c-ares event thread, where the executor would resume this coroutine inline.
        co_await resolver->host_by_name(coro::net::hostname{"libcoro-regression.invalid"});
        // Destroying the channel from inside its own callback double-frees the query (Linux) or self-joins (Windows).
        resolver.reset();
        co_return std::this_thread::get_id();
    };

    auto continuation_thread = coro::sync_wait(lookup());
    executor->shutdown();
    REQUIRE(resolver == nullptr);
    REQUIRE(continuation_thread == pool_thread);
}

#endif // LIBCORO_FEATURE_NETWORKING
