#pragma once
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include "packet_codec.hpp"

namespace coro::net
{

class packet_reader
{
public:
    explicit packet_reader(std::span<const std::byte> payload_data) : m_payload_data(payload_data) {}

    auto peek(std::span<std::byte> buffer) const -> void
    {
        if (buffer.size() > m_payload_data.size() - m_cursor)
        {
            throw std::invalid_argument("");
        }

        std::memcpy(buffer.data(), m_payload_data.data(), buffer.size());
    }

    auto read(std::span<std::byte> buffer) -> void
    {
        peek(buffer);
        m_cursor += buffer.size();
    }

private:
    std::span<const std::byte> m_payload_data;
    std::size_t                m_cursor{};
};

template<packet_deserialisable T>
packet_reader& operator>>(packet_reader& reader, T &deserialisable)
{
    packet_codec<T>::deserialise(reader, deserialisable);
    return reader;
}

} // namespace coro::net