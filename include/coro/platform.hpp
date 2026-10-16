#pragma once

// On supported targets exactly one of CORO_PLATFORM_LINUX, CORO_PLATFORM_BSD (BSDs and Apple) or
// CORO_PLATFORM_WINDOWS is defined; none is defined elsewhere (e.g. Emscripten).
// CORO_PLATFORM_UNIX is additionally defined for Linux and BSD.

#if defined(__linux__)
    #define CORO_PLATFORM_LINUX 1
    #define CORO_PLATFORM_UNIX  1
#elif defined(__FreeBSD__) || defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #define CORO_PLATFORM_BSD  1
    #define CORO_PLATFORM_UNIX 1
#elif defined(_WIN32)
    #define CORO_PLATFORM_WINDOWS 1
#endif
