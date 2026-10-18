#pragma once

// <version> defines the feature test macro; without it the check below is evaluated before any standard
// library header was seen, so the alias resolved per translation unit depending on the include order
// (a silent ODR mismatch that MSVC reports as an unresolved return type at link time).
#include <version>

#if defined(__cpp_lib_expected)
    #include <expected>
namespace coro
{
template<typename T, typename E>
using expected = std::expected<T, E>;

template<typename E>
using unexpected = std::unexpected<E>;
} // namespace coro
#else
    #include "coro/detail/tl_expected.hpp"
namespace coro
{
template<typename T, typename E>
using expected = tl::expected<T, E>;

template<typename E>
using unexpected = tl::unexpected<E>;
} // namespace coro
#endif
