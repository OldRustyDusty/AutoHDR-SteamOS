// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

// Game Mode hides stderr, so every message also goes to
// ~/.local/state/hdrket/hdrket-<pid>.log (the newest few are kept).
static FILE* logFile()
{
    static FILE* file = [] () -> FILE* {
        const char* state = std::getenv("XDG_STATE_HOME");
        const char* home  = std::getenv("HOME");
        std::string dir   = state && *state ? std::string(state) + "/hdrket"
                                            : std::string(home ? home : "/tmp") + "/.local/state/hdrket";
        for (size_t i = 1; i <= dir.size(); i++)
            if (i == dir.size() || dir[i] == '/')
                mkdir(dir.substr(0, i).c_str(), 0755);
        // Keep the directory small: drop all but the newest nine logs.
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> logs;
        for (auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().filename().string().rfind("hdrket-", 0) == 0 && e.path().extension() == ".log")
                logs.push_back(e);
        std::sort(logs.begin(), logs.end(), [](auto& a, auto& b) {
            std::error_code e1, e2;
            return a.last_write_time(e1) > b.last_write_time(e2);
        });
        for (size_t i = 9; i < logs.size(); i++)
            std::filesystem::remove(logs[i].path(), ec);
        std::string path = dir + "/hdrket-" + std::to_string(getpid()) + ".log";
        FILE*       f    = std::fopen(path.c_str(), "we");
        if (f)
            std::setvbuf(f, nullptr, _IOLBF, 0);
        return f;
    }();
    return file;
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
    static std::mutex lock;
    std::lock_guard   l(lock);
    if (FILE* f = logFile())
        std::fprintf(f, "[hdrket %s] %s\n", names[static_cast<int>(lvl)], buf);
}

} // namespace hdrket::log
