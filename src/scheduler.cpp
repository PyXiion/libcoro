#include "coro/scheduler.hpp"
#include "coro/detail/task_self_deleting.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;

namespace coro
{

namespace detail
{
static auto
    make_spawned_joinable_wait_task(std::unique_ptr<coro::task_group<coro::scheduler>> group_ptr) -> coro::task<void>
{
    co_await *group_ptr;
    co_return;
}

} // namespace detail

scheduler::scheduler(options&& opts, private_constructor)
    : m_opts(opts),
      m_io_notifier(),
      m_timer(static_cast<const void*>(&m_timer_object), m_io_notifier)
{
    m_recent_events.reserve(m_max_events);

    if (m_opts.execution_strategy == execution_strategy_t::process_tasks_on_thread_pool)
    {
        m_thread_pool = thread_pool::make_unique(std::move(m_opts.pool));
    }
}

auto scheduler::make_unique(options opts) -> std::unique_ptr<scheduler>
{
    auto s = std::make_unique<scheduler>(std::move(opts), private_constructor{});

    // Spawn the dedicated event loop thread once the scheduler is fully constructed
    // so it has a full object to work with.
    if (s->m_opts.thread_strategy == thread_strategy_t::spawn)
    {
        s->m_io_thread = std::thread([s = s.get()]() { s->process_events_dedicated_thread(); });
    }
    // else manual mode, the user must call process_events.

    return s;
}

scheduler::~scheduler()
{
    shutdown();

    if (m_io_thread.joinable())
    {
        m_io_thread.join();
    }
}

auto scheduler::process_events(std::chrono::milliseconds timeout) -> std::size_t
{
    process_events_manual(timeout);
    return size();
}

auto scheduler::spawn_detached(coro::task<void>&& task) -> bool
{
    m_size.fetch_add(1, std::memory_order::release);
    auto wrapper_task = detail::make_task_self_deleting(std::move(task));
    wrapper_task.promise().user_final_suspend([this]() -> void { m_size.fetch_sub(1, std::memory_order::release); });
    return resume(wrapper_task.handle());
}

auto scheduler::spawn_joinable(coro::task<void>&& task) -> coro::task<void>
{
    auto group_ptr = std::make_unique<coro::task_group<coro::scheduler>>(this, std::move(task));
    return detail::make_spawned_joinable_wait_task(std::move(group_ptr));
}

auto scheduler::schedule_at(time_point time) -> coro::task<void>
{
    return yield_until(time);
}

auto scheduler::yield_until(time_point time) -> coro::task<void>
{
    auto now = clock::now();
    if (time <= now)
    {
        co_await schedule();
    }
    else
    {
        co_await operation<detail::async_op_kind::timer>{
            *this, net::invalid_native_handle, nullptr, 0, nullptr, time - now, {}};
    }
}

auto scheduler::poll(fd_t fd, coro::poll_op op, std::chrono::milliseconds timeout, std::stop_token stop)
    -> coro::task<poll_status>
{
    co_return co_await operation<detail::async_op_kind::poll>{*this, fd, nullptr, 0, nullptr, timeout, stop, op};
}

auto scheduler::resume(std::coroutine_handle<> handle) -> bool
{
    if (handle == nullptr || handle.done())
    {
        return false;
    }

    if (m_shutdown_requested.load(std::memory_order::acquire))
    {
        return false;
    }

    if (m_opts.execution_strategy == execution_strategy_t::process_tasks_inline)
    {
        auto* schedule_op        = new schedule_operation{*this};
        schedule_op->m_allocated = true;
        schedule_op->await_suspend(handle);
        return true;
    }
    else
    {
        return m_thread_pool->resume(handle);
    }
}

auto scheduler::shutdown() noexcept -> void
{
    // Only allow shutdown to occur once.
    if (m_shutdown_requested.exchange(true, std::memory_order::acq_rel) == false)
    {
        // Signal the event loop to stop asap.
        m_io_notifier.wake();

        if (m_io_thread.joinable())
        {
            m_io_thread.join();
        }

        if (m_thread_pool != nullptr)
        {
            m_thread_pool->shutdown();
        }
    }
}

auto scheduler::yield_for_internal(std::chrono::nanoseconds amount) -> coro::task<void>
{
    if (amount <= 0ns)
    {
        co_await schedule();
    }
    else
    {
        co_await operation<detail::async_op_kind::timer>{
            *this, net::invalid_native_handle, nullptr, 0, nullptr, amount, {}};
    }
}

auto scheduler::process_events_manual(std::chrono::milliseconds timeout) -> void
{
    bool expected{false};
    if (m_io_processing.compare_exchange_strong(expected, true, std::memory_order::release, std::memory_order::relaxed))
    {
        process_events_execute(timeout);
        m_io_processing.exchange(false, std::memory_order::release);
    }
}

auto scheduler::process_events_dedicated_thread() -> void
{
    if (m_opts.on_io_thread_start_functor != nullptr)
    {
        m_opts.on_io_thread_start_functor();
    }

    m_io_processing.exchange(true, std::memory_order::release);
    // Execute tasks until stopped or there are no more tasks to complete.
    while (!m_shutdown_requested.load(std::memory_order::acquire) || size() > 0)
    {
        process_events_execute(m_default_timeout);
    }
    m_io_processing.exchange(false, std::memory_order::release);

    if (m_opts.on_io_thread_stop_functor != nullptr)
    {
        m_opts.on_io_thread_stop_functor();
    }
}

auto scheduler::process_events_execute(std::chrono::milliseconds timeout) -> void
{
    // Clear the recent events without decreasing the allocated capacity to reduce allocations
    m_recent_events.clear();
    m_io_notifier.next_events(m_recent_events, timeout);

    {
        std::scoped_lock lock{m_publication_mutex};
        for (auto& [op, status] : m_recent_events)
        {
            if (op == m_timer_ptr)
            {
                process_timeout_execute();
            }
            else if (op == ::coro::io_notifier::wake_tag)
            {
                process_scheduled_execute_inline();
                process_cancel_requests();
            }
            else if (!(op->m_state.load(std::memory_order::acquire) & detail::async_operation::state_completed))
            {
                if (m_io_notifier.complete_ready(*op, status))
                {
                    mark_completed(*op);
                }
            }
        }
    }

    // Finish only after dispatch, so duplicate entries in this batch cannot outlive their operation.
    for (auto* op : m_ops_to_finish)
    {
        auto previous = op->m_state.fetch_or(detail::async_operation::state_finished, std::memory_order::acq_rel);
        if (previous & detail::async_operation::state_suspended)
        {
            m_handles_to_resume.emplace_back(op->m_awaiting_coroutine);
        }
    }
    m_ops_to_finish.clear();

    if (!m_handles_to_resume.empty())
    {
        if (m_opts.execution_strategy == execution_strategy_t::process_tasks_inline)
        {
            std::size_t resumed{0};
            for (auto& handle : m_handles_to_resume)
            {
                handle.resume();
                ++resumed;
            }
            if (resumed > 0)
            {
                m_size.fetch_sub(resumed, std::memory_order::release);
            }
        }
        else
        {
            m_thread_pool->resume(m_handles_to_resume);
            m_size.fetch_sub(m_handles_to_resume.size(), std::memory_order::release);
        }

        m_handles_to_resume.clear();
    }
}

auto scheduler::process_scheduled_execute_inline() -> void
{
    // Allow new additions to trigger a wake-up before acquiring the scheduled operations.
    m_schedule_triggered.exchange(false, std::memory_order::release);

    // Now it is safe to acquire all scheduled ops.
    auto* ops = detail::awaiter_list_pop_all(m_scheduled_ops);

    if (ops != nullptr)
    {
        ops = detail::awaiter_list_reverse(ops);

        while (ops != nullptr)
        {
            auto* next = ops->m_next;
            m_handles_to_resume.emplace_back(ops->m_awaiting_coroutine);

            if (ops->m_allocated)
            {
                delete ops;
            }

            ops = next;
        }
    }
}

auto scheduler::request_cancel(detail::async_operation& op) noexcept -> void
{
    auto previous = op.m_state.fetch_or(detail::async_operation::state_cancel_queued, std::memory_order::acq_rel);
    if (previous & (detail::async_operation::state_cancel_queued | detail::async_operation::state_completed))
    {
        return;
    }
    detail::awaiter_list_push(m_cancel_requests, &op);
    m_io_notifier.wake();
}

auto scheduler::process_cancel_requests() -> void
{
    auto* ops = detail::awaiter_list_pop_all(m_cancel_requests);
    while (ops != nullptr)
    {
        auto* next = ops->m_next;
        if (ops->m_state.load(std::memory_order::acquire) & detail::async_operation::state_completed)
        {
            m_ops_to_finish.emplace_back(ops);
        }
        else
        {
            abort_op(
                *ops,
                ops->m_submit_error != 0 ? net::make_io_status_from_native(ops->m_submit_error)
                                         : net::io_status{net::io_status::kind::cancelled});
            m_ops_to_finish.emplace_back(ops);
        }
        ops = next;
    }
}

auto scheduler::abort_op(detail::async_operation& op, net::io_status status) -> void
{
    if (op.m_state.load(std::memory_order::acquire) & detail::async_operation::state_completed)
    {
        return;
    }
    if (op.m_kind != detail::async_op_kind::timer)
    {
        m_io_notifier.cancel(op);
    }
    op.m_status = status;
    if (op.m_kind == detail::async_op_kind::poll)
    {
        switch (status.type)
        {
            case net::io_status::kind::timeout:
                op.m_poll_status = poll_status::timeout;
                break;
            case net::io_status::kind::cancelled:
                op.m_poll_status = poll_status::cancelled;
                break;
            default:
                op.m_poll_status = poll_status::error;
                break;
        }
    }
    mark_completed(op);
}

auto scheduler::mark_completed(detail::async_operation& op) -> void
{
    remove_timer_token(op);
    auto previous = op.m_state.fetch_or(detail::async_operation::state_completed, std::memory_order::acq_rel);
    if (!(previous & detail::async_operation::state_cancel_queued))
    {
        m_ops_to_finish.emplace_back(&op);
    }
}

auto scheduler::process_timeout_execute() -> void
{
    std::vector<detail::async_operation*> expired;
    {
        std::scoped_lock lock{m_timed_events_mutex};
        auto             now = clock::now();
        while (!m_timed_events.empty() && m_timed_events.begin()->first <= now)
        {
            auto  first = m_timed_events.begin();
            auto* op    = first->second;
            op->m_timer_pos.reset();
            m_timed_events.erase(first);
            expired.emplace_back(op);
        }
        update_timeout(clock::now());
    }
    for (auto* op : expired)
    {
        abort_op(
            *op,
            {op->m_kind == detail::async_op_kind::timer ? net::io_status::kind::ok : net::io_status::kind::timeout});
    }
}

auto scheduler::add_timer_token(time_point tp, detail::async_operation& op) -> void
{
    std::scoped_lock lock{m_timed_events_mutex};
    op.m_timer_pos = m_timed_events.emplace(tp, &op);
    if (*op.m_timer_pos == m_timed_events.begin())
    {
        update_timeout(clock::now());
    }
}

auto scheduler::remove_timer_token(detail::async_operation& op) -> void
{
    std::scoped_lock lock{m_timed_events_mutex};
    if (op.m_timer_pos)
    {
        auto is_first = *op.m_timer_pos == m_timed_events.begin();
        m_timed_events.erase(*op.m_timer_pos);
        op.m_timer_pos.reset();
        if (is_first)
        {
            update_timeout(clock::now());
        }
    }
}

auto scheduler::update_timeout(time_point now) -> void
{
    if (!m_timed_events.empty())
    {
        auto& [tp, pi] = *m_timed_events.begin();

        auto amount = tp - now;

        if (!m_io_notifier.watch_timer(m_timer, amount))
        {
            std::cerr << "Failed to set timerfd errorno=[" << std::string{strerror(errno)} << "].";
        }
    }
    else
    {
        m_io_notifier.unwatch_timer(m_timer);
    }
}

} // namespace coro
