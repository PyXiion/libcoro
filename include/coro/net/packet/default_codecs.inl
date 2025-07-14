#pragma once
#include <bit>
#include <cstdint>
#include <span>
#include <string>

namespace coro::net
{
class packet;
class packet_reader;

template<typename T>
class packet_codec;

template<std::integral T>
class packet_codec<T>
{
public:
    auto serialise(packet& packet, T value) -> void
    {
        convert(value);
        packet.write(std::as_bytes(std::span{&value, 1}));
    }

    auto deserialise(packet_reader& reader, T& value) -> void
    {
        reader.read(std::as_writable_bytes(std::span{&value, 1}));
        convert(value);
    }

private:
    /**
     * ntoh/hton
     */
    constexpr static auto convert(T& value) -> void
    {
        if constexpr (std::endian::native != std::endian::big)
        {
            auto bytes = std::as_writable_bytes(std::span{&value, 1});
            for (int i = 0; i < bytes.size() / 2; ++i)
            {
                std::swap(bytes[i], bytes[bytes.size() - i - 1]);
            }
        }
    }
};

template<std::floating_point T>
class packet_codec<T>
{
    auto serialise(packet& packet, T value) -> void
    {
        packet.write(std::as_bytes(std::span{&value, 1}));
    }

    auto deserialise(packet_reader& reader, T& value) -> void
    {
        reader.read(std::as_writable_bytes(std::span{&value, 1}));
    }
};

template<>
class packet_codec<std::string>
{
    // std::size_t or std::string::size_t are not cross-platform
    using string_size_t = std::uint32_t;

public:
    auto serialise(packet& packet, const std::string& str)
    {
        packet << static_cast<string_size_t>(str.size());

        packet.write(std::as_bytes(std::span{str.data(), str.size()}));
    }
    auto deserialise(packet_reader& reader, std::string& str)
    {
        string_size_t size{};
        reader >> size;
        str.resize(size);
        reader.read(std::as_writable_bytes(std::span{str.data(), str.size()}));
    }
};
} // namespace coro::net