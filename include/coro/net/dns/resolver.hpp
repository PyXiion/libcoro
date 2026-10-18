#pragma once

#include "coro/concepts/executor.hpp"
#include "coro/event.hpp"
#include "coro/net/detail/socket_api.hpp"
#include "coro/net/hostname.hpp"
#include "coro/net/ip_address.hpp"
#include "coro/task.hpp"

#include <ares.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace coro::net::dns
{
namespace detail
{
// One library lifetime shared by every resolver specialization and translation unit.
class ares_library_guard
{
public:
    ares_library_guard()
    {
        std::scoped_lock lock{m_mutex};
        if (m_count == 0)
        {
            if (!ares_threadsafety())
            {
                throw std::runtime_error{"dns resolver requires c-ares thread support"};
            }
#if defined(CORO_PLATFORM_WINDOWS)
            WSADATA    data{};
            const auto error = WSAStartup(MAKEWORD(2, 2), &data);
            if (error != 0)
            {
                throw std::system_error{error, std::system_category(), "dns resolver WSAStartup"};
            }
#endif
            const auto status = ares_library_init(ARES_LIB_INIT_ALL);
            if (status != ARES_SUCCESS)
            {
#if defined(CORO_PLATFORM_WINDOWS)
                WSACleanup();
#endif
                throw std::runtime_error{ares_strerror(status)};
            }
        }
        ++m_count;
    }

    ares_library_guard(const ares_library_guard&)                    = delete;
    ares_library_guard(ares_library_guard&&)                         = delete;
    auto operator=(const ares_library_guard&) -> ares_library_guard& = delete;
    auto operator=(ares_library_guard&&) -> ares_library_guard&      = delete;

    ~ares_library_guard()
    {
        std::scoped_lock lock{m_mutex};
        if (--m_count == 0)
        {
            ares_library_cleanup();
#if defined(CORO_PLATFORM_WINDOWS)
            WSACleanup();
#endif
        }
    }

private:
    inline static std::mutex  m_mutex{};
    inline static std::size_t m_count{0};
};
} // namespace detail

template<concepts::executor executor_type>
class resolver;

enum class status
{
    complete,
    error
};

class result
{
    template<concepts::executor executor_type>
    friend class resolver;

public:
    [[nodiscard]] auto is_ok() const noexcept -> bool { return m_status == dns::status::complete; }

    [[nodiscard]] auto is_error() const noexcept -> bool { return m_status == dns::status::error; }

    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] auto status() const noexcept -> dns::status { return m_status; }

    [[nodiscard]] auto ip_addresses() const noexcept -> const std::vector<net::ip_address>& { return m_ip_addresses; }

private:
    dns::status                  m_status{dns::status::error};
    std::vector<net::ip_address> m_ip_addresses{};
};

template<concepts::executor executor_type>
class resolver
{
public:
    explicit resolver(std::unique_ptr<executor_type>& executor) : m_executor(executor)
    {
        if (m_executor == nullptr)
        {
            throw std::runtime_error{"dns resolver cannot have nullptr executor"};
        }

        ares_options options{};
        options.evsys = ARES_EVSYS_DEFAULT;

        const auto channel_init_status = ares_init_options(&m_ares_channel, &options, ARES_OPT_EVENT_THREAD);
        if (channel_init_status != ARES_SUCCESS)
        {
            throw std::runtime_error{ares_strerror(channel_init_status)};
        }
    }

    resolver(const resolver&)                             = delete;
    resolver(resolver&&)                                  = delete;
    auto operator=(const resolver&) noexcept -> resolver& = delete;
    auto operator=(resolver&&) noexcept -> resolver&      = delete;

    ~resolver()
    {
        if (m_ares_channel != nullptr)
        {
            ares_destroy(m_ares_channel);
            m_ares_channel = nullptr;
        }
    }

    /**
     * Resolves the hostname to IP addresses and returns owned lookup data.
     * The hostname is owned by the task, including before it is first awaited.
     * Keep each started task frame alive until completion. The resolver, executor, and
     * unique_ptr owning the executor must outlive every started lookup; do not reset that owner
     * or shut down the executor while a lookup is pending.
     */
    auto host_by_name(net::hostname hn) -> coro::task<dns::result>
    {
        request_state request{m_executor};

        ares_addrinfo_hints hints{};
        hints.ai_family = AF_UNSPEC; // Request both IPv4 and IPv6

        ares_getaddrinfo(
            m_ares_channel,
            hn.data().data(),
            nullptr, // service name (port number or NULL)
            &hints,
            ares_dns_callback,
            &request);

        co_await request.m_resume;
        if (request.m_exception != nullptr)
        {
            std::rethrow_exception(request.m_exception);
        }
        co_return std::move(request.m_result);
    }

private:
    struct request_state
    {
        std::unique_ptr<executor_type>& m_executor;
        coro::event                     m_resume{};
        dns::result                     m_result{};
        std::exception_ptr              m_exception{};
    };

    /// Executor used to resume lookups completed by the c-ares event thread.
    std::unique_ptr<executor_type>& m_executor;

    detail::ares_library_guard m_library{};

    /// The libc-ares channel for looking up dns entries.
    ares_channel m_ares_channel{nullptr};

    static auto ares_dns_callback(void* arg, int status, int /*timeouts*/, ares_addrinfo* addr_info) noexcept -> void
    {
        auto& request = *static_cast<request_state*>(arg);
        auto& result  = request.m_result;

        try
        {
            if (addr_info != nullptr && status == ARES_SUCCESS)
            {
                for (const auto* node = addr_info->nodes; node != nullptr; node = node->ai_next)
                {
                    if (node->ai_family == AF_INET)
                    {
                        const auto* address = reinterpret_cast<const sockaddr_in*>(node->ai_addr);
                        result.m_ip_addresses.emplace_back(
                            std::span<const uint8_t>{
                                reinterpret_cast<const uint8_t*>(&address->sin_addr), net::ip_address::ipv4_len},
                            net::domain_t::ipv4);
                    }
                    else if (node->ai_family == AF_INET6)
                    {
                        const auto* address = reinterpret_cast<const sockaddr_in6*>(node->ai_addr);
                        result.m_ip_addresses.emplace_back(
                            std::span<const uint8_t>{
                                reinterpret_cast<const uint8_t*>(&address->sin6_addr), net::ip_address::ipv6_len},
                            net::domain_t::ipv6);
                    }
                }
                result.m_status = dns::status::complete;
            }
        }
        catch (...)
        {
            // No C++ exception may escape through c-ares' C callback.
            request.m_exception = std::current_exception();
        }
        if (addr_info != nullptr)
        {
            ares_freeaddrinfo(addr_info);
        }

        // Resumption may destroy request; do not access it after signaling.
        request.m_resume.set(request.m_executor, resume_order_policy::lifo);
    }
};

} // namespace coro::net::dns
