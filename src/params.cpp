// SPDX-License-Identifier: MIT
//
// Live settings, per-game presets, and the stats the UI reads.

#include "params.hpp"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hdrket {

namespace {

std::mutex            g_lock; // guards everything below
Settings              g_settings;
Stats                 g_stats;
SwapchainInfo         g_swapchain;
std::atomic<uint64_t> g_version{1};

const char* eotfName(Eotf e)
{
    switch (e) {
    case Eotf::SRGB: return "srgb";
    case Eotf::Gamma22: return "gamma22";
    case Eotf::Gamma24: return "gamma24";
    case Eotf::Linear: return "linear";
    }
    return "gamma22";
}

bool parseEotf(const std::string& v, Eotf* out)
{
    if (v == "srgb") *out = Eotf::SRGB;
    else if (v == "gamma22" || v == "2.2") *out = Eotf::Gamma22;
    else if (v == "gamma24" || v == "2.4") *out = Eotf::Gamma24;
    else if (v == "linear") *out = Eotf::Linear;
    else return false;
    return true;
}

Settings clamped(Settings s)
{
    s.peakNits      = std::clamp(s.peakNits, 80.0f, 10000.0f);
    s.greyNits      = std::clamp(s.greyNits, 5.0f, 200.0f);
    s.expansion     = std::clamp(s.expansion, 0.0f, 1.0f);
    s.saturation    = std::clamp(s.saturation, 0.0f, 2.0f);
    s.blackFloor    = std::clamp(s.blackFloor, 0.0f, 0.2f);
    s.shoulderStart = std::clamp(s.shoulderStart, 0.0f, 0.95f);
    s.shoulderPow   = std::clamp(s.shoulderPow, 0.5f, 8.0f);
    s.split         = std::clamp(s.split, 0.0f, 1.0f);
    return s;
}

std::string configDir()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return std::string(xdg) + "/hdrket";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/hdrket";
}

std::string presetPath()
{
    return configDir() + "/presets/" + gameKey() + ".conf";
}

void mkdirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0755);
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// Applies one `key = value` pair. Shared by preset files and env vars.
bool applyKey(Settings& s, const std::string& key, const std::string& value)
{
    auto num = [&](float* f) {
        char* end = nullptr;
        float v   = std::strtof(value.c_str(), &end);
        if (end == value.c_str())
            return false;
        *f = v;
        return true;
    };
    auto flag = [&](bool* b) {
        *b = value == "true" || value == "1";
        return true;
    };
    if (key == "peak_nits") return num(&s.peakNits);
    if (key == "middle_grey_nits") return num(&s.greyNits);
    if (key == "expansion") return num(&s.expansion);
    if (key == "saturation") return num(&s.saturation);
    if (key == "black_floor") return num(&s.blackFloor);
    if (key == "eotf") return parseEotf(value, &s.eotf);
    if (key == "shoulder_start") return num(&s.shoulderStart);
    if (key == "shoulder_pow") return num(&s.shoulderPow);
    if (key == "split") return num(&s.split);
    if (key == "pattern") return flag(&s.pattern);
    if (key == "bypass") return flag(&s.bypass);
    if (key == "paper_white_nits") {
        float pw;
        if (!num(&pw))
            return false;
        s.greyNits = pw * 0.18f;
        return true;
    }
    return false;
}

bool loadPresetFile(const std::string& path, Settings& s)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    int         n = 0;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find('#')));
        size_t eq = line.find('=');
        if (line.empty() || eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        if (applyKey(s, key, val))
            n++;
        else
            HK_WARN("%s: unknown setting '%s'", path.c_str(), key.c_str());
    }
    HK_INFO("preset loaded: %s (%d settings)", path.c_str(), n);
    return true;
}

} // namespace

ItmParams toPushConstants(const Settings& s)
{
    ItmParams p{};
    p.peakNits       = s.peakNits;
    p.paperWhiteNits = s.paperWhiteNits();
    p.expansion      = s.expansion;
    p.saturation     = s.saturation;
    p.blackFloor     = s.blackFloor;
    p.shoulderStart  = s.shoulderStart;
    p.shoulderPow    = s.shoulderPow;
    p.splitX         = s.split;
    p.flags          = (s.bypass ? ITM_BYPASS : 0u) | (s.pattern ? ITM_PATTERN : 0u);
    p.eotf           = static_cast<uint32_t>(s.eotf);
    return p;
}

Settings currentSettings()
{
    std::lock_guard l(g_lock);
    return g_settings;
}

void updateSettings(const Settings& s)
{
    std::lock_guard l(g_lock);
    g_settings = clamped(s);
    g_version++;
}

uint64_t settingsVersion()
{
    return g_version.load();
}

const std::string& gameKey()
{
    static const std::string key = [] {
        std::string name;
        if (const char* preset = std::getenv("HDRKET_PRESET"); preset && *preset) {
            name = preset;
        } else {
            // Proton: the game is the last *.exe on the command line.
            std::ifstream     f("/proc/self/cmdline", std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string all = ss.str();
            for (size_t pos = 0; pos < all.size();) {
                std::string arg = all.c_str() + pos;
                pos += arg.size() + 1;
                std::string lower = arg;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0)
                    name = arg;
            }
            if (name.empty()) {
                char    buf[4096];
                ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
                name      = n > 0 ? std::string(buf, n) : "unknown";
            }
            name = name.substr(name.find_last_of("/\\") + 1);
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".exe") == 0)
                name.resize(name.size() - 4);
        }
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
                c = '_';
        return name;
    }();
    return key;
}

void loadInitialSettings()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Settings s;
        loadPresetFile(presetPath(), s);

        // Environment overrides, e.g. HDRKET_PEAK_NITS=600 -> peak_nits.
        static const char* keys[] = {"peak_nits", "middle_grey_nits", "paper_white_nits", "expansion", "saturation",
                                     "black_floor", "eotf", "shoulder_start", "shoulder_pow", "split", "pattern",
                                     "bypass"};
        for (const char* key : keys) {
            std::string env = "HDRKET_";
            for (const char* c = key; *c; c++)
                env += static_cast<char>(std::toupper(*c));
            if (const char* v = std::getenv(env.c_str()); v && *v)
                applyKey(s, key, v);
        }
        updateSettings(s);
        s = currentSettings();
        HK_INFO("game \"%s\": peak %.0f nits, middle grey %.0f nits (SDR white %.0f), expansion %.2f, saturation %.2f, "
                "black floor %.3f, EOTF %s",
                gameKey().c_str(), s.peakNits, s.greyNits, s.paperWhiteNits(), s.expansion, s.saturation, s.blackFloor,
                eotfName(s.eotf));
    });
}

bool savePreset(std::string* pathOut)
{
    Settings    s    = currentSettings();
    std::string path = presetPath();
    mkdirs(path.substr(0, path.find_last_of('/')));
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            return false;
        out << "# HDR Ket preset for " << gameKey() << "\n"
            << "peak_nits = " << s.peakNits << "\n"
            << "middle_grey_nits = " << s.greyNits << "\n"
            << "expansion = " << s.expansion << "\n"
            << "saturation = " << s.saturation << "\n"
            << "black_floor = " << s.blackFloor << "\n"
            << "eotf = \"" << eotfName(s.eotf) << "\"\n"
            << "shoulder_start = " << s.shoulderStart << "\n"
            << "shoulder_pow = " << s.shoulderPow << "\n";
        if (!out)
            return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    if (pathOut)
        *pathOut = path;
    HK_INFO("preset saved: %s", path.c_str());
    return true;
}

void publishStats(const Stats& s)
{
    std::lock_guard l(g_lock);
    g_stats = s;
}

Stats latestStats()
{
    std::lock_guard l(g_lock);
    return g_stats;
}

void publishSwapchainInfo(const SwapchainInfo& info)
{
    std::lock_guard l(g_lock);
    g_swapchain = info;
}

SwapchainInfo latestSwapchainInfo()
{
    std::lock_guard l(g_lock);
    return g_swapchain;
}

const char* settingsEotfName(Eotf e)
{
    return eotfName(e);
}

bool settingsApplyKey(Settings& s, const std::string& key, const std::string& value)
{
    return applyKey(s, key, value);
}

} // namespace hdrket
