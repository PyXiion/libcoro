#include "catch_amalgamated.hpp"
#include <coro/coro.hpp>

TEST_CASE("nqueens", "[.ram]")
{
}

constexpr static std::size_t skynet_max_depth = 8;

auto skynet(coro::thread_pool& pool, std::size_t base_num = 0, std::size_t depth = 0) -> coro::task<std::size_t>
{
    co_await pool.schedule();

    if (depth == skynet_max_depth)
    {
        co_return base_num;
    }

    std::size_t depth_offset = 1;
    for (std::size_t i = 0; i < skynet_max_depth - depth - 1; ++i)
    {
        depth_offset *= 10;
    }

    auto results = co_await coro::when_all(
        std::ranges::views::iota(0UL, 10UL) |
        std::ranges::views::transform([=, &pool](std::size_t idx)
                                      { return skynet(pool, base_num + depth_offset * idx, depth + 1); }));

    std::size_t count = 0;
    for (std::size_t idx = 0; idx < 10; ++idx)
    {
        count += results[idx].return_value();
    }

    co_return count;
}

TEST_CASE("skynet", "[.ram]")
{
    auto thread_pool = coro::thread_pool::make_unique();

    coro::sync_wait(skynet(*thread_pool));
}