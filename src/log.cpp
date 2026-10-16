// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hdrket::log {

static Level threshold()
{
    static const Level lvl = [] {
        const char* env = std::getenv("HDRKET_LOG");
        if (!env)
            return Level::Info;
        if (!std::strcmp(env, "error"))
            return Level::Error;
        if (!std::strcmp(env, "warn"))
            return Level::Warn;
        if (!std::strcmp(env, "debug"))
            return Level::Debug;
        return Level::Info;
    }();
    return lvl;
}

bool enabled(Level lvl)
{
    return static_cast<int>(lvl) <= static_cast<int>(threshold());
}

void write(Level lvl, const char* fmt, ...)
{
    if (!enabled(lvl))
        return;
    static const char* names[] = {"error", "warn", "info", "debug"};
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[hdrket %s] %s\n", names[static_cast<int>(lvl)], buf);
}

} // namespace hdrket::log
