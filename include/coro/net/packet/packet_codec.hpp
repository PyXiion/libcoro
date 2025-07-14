#pragma once
#include <concepts>

namespace coro::net
{
class packet;
class packet_reader;

template<class T>
class packet_codec;

template<class T>
concept packet_serialisable = requires(packet& packet, const T& value) {
    { packet_codec<T>::serialise(packet, value) } -> std::same_as<void>;
};

template<class T>
concept packet_deserialisable = requires(packet_reader& packet_reader, T &value) {
    { packet_codec<T>::deserialise(packet_reader, value) } -> std::convertible_to<T>;
};

} // namespace coro::net