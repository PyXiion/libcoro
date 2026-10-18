#include "catch_amalgamated.hpp"

#ifdef LIBCORO_FEATURE_NETWORKING

    #include <coro/coro.hpp>

    #include <future>
    #include <memory>
    #include <utility>
    #include <vector>

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

#endif // LIBCORO_FEATURE_NETWORKING
