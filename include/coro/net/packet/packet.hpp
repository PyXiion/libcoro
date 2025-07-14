#pragma once
#include "packet_reader.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace coro::net
{

class packet
{
    using packet_size = std::uint32_t;

    static constexpr auto metadata_size = sizeof(packet_size);

public:
    explicit packet(const std::size_t size) : m_data{metadata_size} { m_data.reserve(m_data.size() + size); }
    explicit packet(const std::span<const std::byte> data) : m_data{metadata_size} { write(data); }

    auto write(const std::span<const std::byte> data) -> void
    {
        const packet_size old_size = m_data.size();
        m_data.resize(old_size + data.size());
        std::memcpy(m_data.data() + old_size, data.data(), data.size());

        set_inner_size(m_data.size());
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool
    {
        return payload_size() == 0;
    }

    [[nodiscard]]
    auto size() const noexcept -> packet_size
    {
        return m_data.size();
    }
    [[nodiscard]]
    auto payload_size() const noexcept -> packet_size
    {
        return m_data.size() - metadata_size;
    }
    [[nodiscard]]
    auto capacity() const noexcept -> packet_size
    {
        return m_data.capacity();
    }

    auto resize(const packet_size payload_size) -> void { m_data.resize(payload_size + metadata_size); }
    auto reserve(const packet_size payload_size) -> void { m_data.reserve(payload_size + metadata_size); }

    [[nodiscard]]
    auto data() noexcept -> std::byte*
    {
        return m_data.data();
    }
    [[nodiscard]]
    auto data() const -> const std::byte*
    {
        return m_data.data();
    }

    [[nodiscard]]
    auto payload_data() noexcept -> std::byte*
    {
        return m_data.data() + metadata_size;
    }
    [[nodiscard]]
    auto payload_data() const -> const std::byte*
    {
        return m_data.data() + metadata_size;
    }

    [[nodiscard]]
    auto reader() const& -> packet_reader
    {
        return packet_reader{std::span{payload_data(), payload_size()}};
    }
    auto reader() const&& -> packet_reader = delete;

private:
    auto set_inner_size(const packet_size size) noexcept -> void { std::memcpy(m_data.data(), &size, sizeof(size)); }

    std::vector<std::byte> m_data;
};

template<packet_serialisable T>
packet& operator<<(packet& packet, T &&serialisable)
{
    packet_codec<T>::serialise(packet, std::forward<T>(serialisable));
    return packet;
}

} // namespace coro::net

#include "default_codecs.inl"