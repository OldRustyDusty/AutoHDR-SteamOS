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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
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
std::atomic<float>    g_displayPeak{1000.0f}; // this display's base peak (EDID or profile)

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

const char* tonemapName(Tonemap t)
{
    switch (t) {
    case Tonemap::Off: return "off";
    case Tonemap::Clip: return "clip";
    case Tonemap::Map: return "map";
    case Tonemap::Adaptive: return "adaptive";
    }
    return "adaptive";
}

bool parseTonemap(const std::string& v, Tonemap* out)
{
    if (v == "off") *out = Tonemap::Off;
    else if (v == "clip") *out = Tonemap::Clip;
    else if (v == "map") *out = Tonemap::Map;
    else if (v == "adaptive") *out = Tonemap::Adaptive;
    else return false;
    return true;
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
    s.expansion     = std::clamp(s.expansion, 0.0f, 1.5f);
    s.gamutExpansion = std::clamp(s.gamutExpansion, 0.0f, 1.0f);
    s.adaptRelease  = std::clamp(s.adaptRelease, 0.1f, 10.0f);
    s.adaptStrength = std::clamp(s.adaptStrength, 0.0f, 1.0f);
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

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

std::string displayProfilePath()
{
    return configDir() + "/displays/" + displayInfo().key + ".conf";
}

std::string sanitize(std::string name)
{
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            c = '_';
    return name;
}

uint32_t crc32(const std::string& data)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        crc ^= b;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string readFile(const std::string& path)
{
    std::ifstream     f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

DisplayInfo parseEdid(const std::string& edid)
{
    DisplayInfo info;
    auto        byte = [&](size_t i) { return i < edid.size() ? static_cast<unsigned char>(edid[i]) : 0u; };

    // Monitor name descriptor (tag 0xFC), else the 3-letter manufacturer ID.
    for (size_t d = 54; d < 126; d += 18) {
        if (byte(d) == 0 && byte(d + 1) == 0 && byte(d + 3) == 0xFC) {
            std::string n;
            for (size_t i = d + 5; i < d + 18 && byte(i) != 0x0A; i++)
                n += static_cast<char>(byte(i));
            info.name = trim(n);
        }
    }
    if (info.name.empty()) {
        unsigned mfg = (byte(8) << 8) | byte(9);
        for (int shift : {10, 5, 0})
            info.name += static_cast<char>('@' + ((mfg >> shift) & 31));
    }

    // CTA-861 extension blocks: HDR static metadata (extended tag 6). Its
    // fourth byte is the desired max luminance code: 50 * 2^(cv / 32) nits.
    for (size_t b = 128; b + 128 <= edid.size(); b += 128) {
        if (byte(b) != 0x02)
            continue;
        size_t dtd = byte(b + 2);
        for (size_t i = b + 4; i < b + dtd && i < b + 128;) {
            unsigned tag = byte(i) >> 5, len = byte(i) & 31;
            if (tag == 7 && len >= 4 && byte(i + 1) == 6 && byte(i + 4) != 0)
                info.edidPeak = 50.0f * std::exp2(float(byte(i + 4)) / 32.0f);
            i += 1 + len;
        }
    }

    char crc[9];
    std::snprintf(crc, sizeof(crc), "%08x", crc32(edid));
    info.key = sanitize(info.name) + "-" + crc;
    return info;
}

void mkdirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0755);
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
    if (key == "tonemap") return parseTonemap(value, &s.tonemap);
    if (key == "gamut_expansion") return num(&s.gamutExpansion);
    if (key == "adapt_release") return num(&s.adaptRelease);
    if (key == "adapt_strength") return num(&s.adaptStrength);
    if (key == "shoulder_start") return num(&s.shoulderStart);
    if (key == "shoulder_pow") return num(&s.shoulderPow);
    if (key == "split") return num(&s.split);
    if (key == "peak_override") {
        bool was = s.peakOverride;
        flag(&s.peakOverride);
        if (was && !s.peakOverride)
            s.peakNits = g_displayPeak.load(); // back to the display's own peak
        return true;
    }
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

using KeyValues = std::vector<std::pair<std::string, std::string>>;

bool readPresetFile(const std::string& path, KeyValues& out)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find('#')));
        size_t eq = line.find('=');
        if (line.empty() || eq == std::string::npos)
            continue;
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        out.emplace_back(trim(line.substr(0, eq)), val);
    }
    HK_INFO("loaded %s", path.c_str());
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
    p.tonemap        = static_cast<uint32_t>(s.tonemap);
    p.gamutExpansion = s.gamutExpansion;
    p.sourcePeakNits = s.peakNits;
    p.sceneWhite     = 1.0f;
    p.shadowGamma    = 1.0f;
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

const DisplayInfo& displayInfo()
{
    static const DisplayInfo info = [] {
        DisplayInfo found;
        // The game can't tell which output its window is on; with one active
        // display (Game Mode, a TV) this is exact. HDRKET_DISPLAY overrides.
        std::error_code ec;
        std::vector<std::filesystem::path> connectors;
        for (auto& e : std::filesystem::directory_iterator("/sys/class/drm", ec))
            connectors.push_back(e.path());
        std::sort(connectors.begin(), connectors.end());
        int active = 0;
        for (auto& c : connectors) {
            if (trim(readFile(c / "status")) != "connected" || trim(readFile(c / "enabled")) != "enabled")
                continue;
            std::string edid = readFile(c / "edid");
            if (edid.size() < 128)
                continue;
            if (active++ == 0)
                found = parseEdid(edid);
        }
        if (active > 1)
            HK_WARN("%d displays active; using \"%s\" for the peak profile (set HDRKET_DISPLAY to choose)", active,
                    found.name.c_str());
        if (const char* env = std::getenv("HDRKET_DISPLAY"); env && *env) {
            found.key  = sanitize(env);
            found.name = env;
        }
        if (found.key.empty()) {
            found.key  = "unknown-display";
            found.name = "Unknown display";
        }
        HK_INFO("display \"%s\" (profile %s, EDID peak %s)", found.name.c_str(), found.key.c_str(),
                found.edidPeak > 0 ? std::to_string(int(found.edidPeak)).c_str() : "not reported");
        return found;
    }();
    return info;
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
        return sanitize(name);
    }();
    return key;
}

void loadInitialSettings()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // Peak: default -> EDID -> display profile -> this game's override
        // for this display (peak_nits@<display>) or for any display (peak_nits).
        // Everything else: default -> game preset. Then HDRKET_* env on top.
        Settings s;
        if (displayInfo().edidPeak > 0)
            s.peakNits = displayInfo().edidPeak;
        KeyValues display;
        readPresetFile(displayProfilePath(), display);
        for (auto& [k, v] : display)
            if (k == "peak_nits")
                applyKey(s, k, v);
        g_displayPeak = s.peakNits;

        KeyValues game;
        readPresetFile(presetPath(), game);
        const std::string mine = "peak_nits@" + displayInfo().key;
        std::string anyDisplayPeak, thisDisplayPeak;
        for (auto& [k, v] : game) {
            if (k == mine)
                thisDisplayPeak = v;
            else if (k == "peak_nits")
                anyDisplayPeak = v;
            else if (k.rfind("peak_nits@", 0) == 0)
                continue; // another display's override
            else if (!applyKey(s, k, v))
                HK_WARN("%s: unknown setting '%s'", presetPath().c_str(), k.c_str());
        }
        if (!thisDisplayPeak.empty() || !anyDisplayPeak.empty()) {
            applyKey(s, "peak_nits", thisDisplayPeak.empty() ? anyDisplayPeak : thisDisplayPeak);
            s.peakOverride = true;
        }

        // Environment overrides, e.g. HDRKET_PEAK_NITS=600 -> peak_nits.
        static const char* keys[] = {"peak_nits", "middle_grey_nits", "paper_white_nits", "expansion", "saturation",
                                     "black_floor", "eotf", "tonemap", "gamut_expansion", "shoulder_start",
                                     "shoulder_pow", "adapt_release", "adapt_strength", "split", "pattern",
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
        HK_INFO("game \"%s\": peak %.0f nits (%s), middle grey %.0f nits (SDR white %.0f), expansion %.2f, saturation %.2f, "
                "black floor %.3f, EOTF %s, tonemap %s, gamut expansion %.2f",
                gameKey().c_str(), s.peakNits, s.peakOverride ? "this game's" : "display's", s.greyNits, s.paperWhiteNits(), s.expansion, s.saturation, s.blackFloor,
                eotfName(s.eotf), tonemapName(s.tonemap), s.gamutExpansion);
    });
}

static bool saveDisplayProfile(float peakNits)
{
    std::string path = displayProfilePath();
    mkdirs(path.substr(0, path.find_last_of('/')));
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            return false;
        out << "# HDR Ket display profile: " << displayInfo().name << "\n"
            << "peak_nits = " << peakNits << "\n";
        if (!out)
            return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    HK_INFO("display profile saved: %s (peak %.0f nits)", path.c_str(), peakNits);
    return true;
}

bool savePreset(std::string* pathOut)
{
    Settings    s    = currentSettings();
    std::string path = presetPath();
    mkdirs(path.substr(0, path.find_last_of('/')));
    // Keep this game's peak overrides for other displays.
    KeyValues   previous;
    readPresetFile(path, previous);
    const std::string mine = "peak_nits@" + displayInfo().key;
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            return false;
        out << "# HDR Ket preset for " << gameKey() << "\n"
            << "# Peak brightness comes from ../displays/ unless overridden per display below\n"
            << "middle_grey_nits = " << s.greyNits << "\n"
            << "expansion = " << s.expansion << "\n"
            << "saturation = " << s.saturation << "\n"
            << "black_floor = " << s.blackFloor << "\n"
            << "eotf = \"" << eotfName(s.eotf) << "\"\n"
            << "tonemap = \"" << tonemapName(s.tonemap) << "\"\n"
            << "gamut_expansion = " << s.gamutExpansion << "\n"
            << "adapt_release = " << s.adaptRelease << "\n"
            << "adapt_strength = " << s.adaptStrength << "\n"
            << "shoulder_start = " << s.shoulderStart << "\n"
            << "shoulder_pow = " << s.shoulderPow << "\n";
        for (auto& [k, v] : previous)
            if (k.rfind("peak_nits@", 0) == 0 && k != mine)
                out << k << " = " << v << "\n";
        if (s.peakOverride)
            out << mine << " = " << s.peakNits << "  # " << displayInfo().name << "\n";
        if (!out)
            return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    if (!s.peakOverride) {
        if (!saveDisplayProfile(s.peakNits))
            return false;
        g_displayPeak = s.peakNits;
    }
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

const char* settingsTonemapName(Tonemap t)
{
    return tonemapName(t);
}

float expansionTargetNits(const Settings& s)
{
    float paper   = s.paperWhiteNits();
    float peakRel = std::max(s.peakNits / paper, 1.0f);
    return paper * (1.0f + (peakRel - 1.0f) * s.expansion);
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
