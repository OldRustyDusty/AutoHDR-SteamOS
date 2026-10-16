// SPDX-License-Identifier: MIT
#pragma once

#include <cstdarg>

namespace hdrket::log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// HDRKET_LOG=error|warn|info|debug (default info). Output goes to stderr,
// which Steam captures with PROTON_LOG or when launched from a terminal.
bool enabled(Level lvl);
void write(Level lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace hdrket::log

#define HK_ERR(...)  ::hdrket::log::write(::hdrket::log::Level::Error, __VA_ARGS__)
#define HK_WARN(...) ::hdrket::log::write(::hdrket::log::Level::Warn, __VA_ARGS__)
#define HK_INFO(...) ::hdrket::log::write(::hdrket::log::Level::Info, __VA_ARGS__)
#define HK_DBG(...)                                                                     \
    do {                                                                                \
        if (::hdrket::log::enabled(::hdrket::log::Level::Debug))                        \
            ::hdrket::log::write(::hdrket::log::Level::Debug, __VA_ARGS__);             \
    } while (0)
