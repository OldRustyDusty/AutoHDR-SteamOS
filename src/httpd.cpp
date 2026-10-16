// SPDX-License-Identifier: MIT
//
// The tuning UI (requirement R6): a tiny HTTP server inside the game process.
// Open http://<this machine>:7331 on a phone or laptop; every change applies
// on the next frame while the game stays full screen and unobstructed.
//
// One thread, one connection at a time, Connection: close. The page polls
// /api/state a few times a second, which is all the "live" it needs.

#include "log.hpp"
#include "params.hpp"

#include "ui_html.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace hdrket {

namespace {

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            continue;
        out += c;
    }
    return out;
}

std::string fmt(const char* f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

std::string stateJson()
{
    Settings      s  = currentSettings();
    Stats         st = latestStats();
    SwapchainInfo sc = latestSwapchainInfo();

    std::string j = "{";
    j += "\"game\":\"" + jsonEscape(gameKey()) + "\",";
    j += "\"version\":" + std::to_string(settingsVersion()) + ",";
    j += "\"settings\":{";
    j += "\"peak_nits\":" + fmt("%.0f", s.peakNits) + ",";
    j += std::string("\"peak_override\":") + (s.peakOverride ? "true" : "false") + ",";
    j += "\"middle_grey_nits\":" + fmt("%.1f", s.greyNits) + ",";
    j += "\"paper_white_nits\":" + fmt("%.0f", s.paperWhiteNits()) + ",";
    j += "\"expansion\":" + fmt("%.3f", s.expansion) + ",";
    j += "\"saturation\":" + fmt("%.3f", s.saturation) + ",";
    j += "\"black_floor\":" + fmt("%.4f", s.blackFloor) + ",";
    j += std::string("\"eotf\":\"") + settingsEotfName(s.eotf) + "\",";
    j += std::string("\"tonemap\":\"") + settingsTonemapName(s.tonemap) + "\",";
    j += "\"gamut_expansion\":" + fmt("%.3f", s.gamutExpansion) + ",";
    j += "\"adapt_release\":" + fmt("%.2f", s.adaptRelease) + ",";
    j += "\"adapt_strength\":" + fmt("%.2f", s.adaptStrength) + ",";
    j += "\"shoulder_start\":" + fmt("%.3f", s.shoulderStart) + ",";
    j += "\"shoulder_pow\":" + fmt("%.3f", s.shoulderPow) + ",";
    j += "\"split\":" + fmt("%.3f", s.split) + ",";
    j += std::string("\"pattern\":") + (s.pattern ? "true" : "false") + ",";
    j += std::string("\"bypass\":") + (s.bypass ? "true" : "false");
    j += "},\"display\":{";
    j += "\"name\":\"" + jsonEscape(displayInfo().name) + "\",";
    j += "\"key\":\"" + jsonEscape(displayInfo().key) + "\",";
    j += "\"edid_peak\":" + fmt("%.0f", displayInfo().edidPeak);
    j += "},\"output\":{";
    j += "\"format\":\"" + jsonEscape(sc.outputFormat) + "\",";
    j += "\"colorspace\":\"" + jsonEscape(sc.outputColorSpace) + "\",";
    j += "\"source\":\"" + jsonEscape(sc.sourceFormat) + "\",";
    j += "\"width\":" + std::to_string(sc.width) + ",\"height\":" + std::to_string(sc.height);
    j += "},\"stats\":{";
    j += "\"avg\":" + fmt("%.1f", st.frameAvgNits) + ",";
    j += "\"peak\":" + fmt("%.0f", st.framePeakNits) + ",";
    j += "\"p99\":" + fmt("%.0f", st.p99Nits) + ",";
    j += "\"held_peak\":" + fmt("%.0f", st.heldPeakNits) + ",";
    j += "\"content_peak\":" + fmt("%.0f", st.contentPeakNits) + ",";
    j += "\"tonemap_source\":" + fmt("%.0f", st.tonemapSourceNits) + ",";
    j += "\"wide_gamut\":" + fmt("%.4f", st.wideGamutFraction) + ",";
    j += "\"scene_white\":" + fmt("%.3f", st.sceneWhite) + ",";
    j += "\"shadow_lift\":" + fmt("%.3f", st.shadowLift) + ",";
    j += "\"frames\":" + std::to_string(st.frames) + ",";
    j += "\"histogram\":[";
    for (int i = 0; i < kHistogramBins; i++)
        j += (i ? "," : "") + fmt("%.4f", st.histogram[i]);
    j += "],\"hist_min_log2\":" + fmt("%.1f", kHistogramMinLog2) + ",\"hist_max_log2\":" + fmt("%.1f", kHistogramMaxLog2);
    j += "}}";
    return j;
}

std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+')
            out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

// Body is application/x-www-form-urlencoded: peak_nits=600&eotf=gamma24
void applyForm(const std::string& body)
{
    Settings s = currentSettings();
    size_t   pos = 0;
    while (pos <= body.size()) {
        size_t      amp  = body.find('&', pos);
        std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t      eq   = pair.find('=');
        if (eq != std::string::npos)
            settingsApplyKey(s, urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1)));
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    updateSettings(s);
}

void sendAll(int fd, const std::string& data)
{
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
            return;
        off += size_t(n);
    }
}

void respond(int fd, int code, const char* type, const std::string& body)
{
    const char* status = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Error";
    std::string head   = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    head += std::string("Content-Type: ") + type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    sendAll(fd, head + body);
}

void handle(int fd)
{
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    char        buf[4096];
    size_t      headerEnd = std::string::npos;
    while (headerEnd == std::string::npos && req.size() < 65536) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        req.append(buf, size_t(n));
        headerEnd = req.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos)
        return;

    std::string method = req.substr(0, req.find(' '));
    size_t      pathAt = method.size() + 1;
    std::string path   = req.substr(pathAt, req.find(' ', pathAt) - pathAt);
    path               = path.substr(0, path.find('?'));

    size_t contentLength = 0;
    for (const char* h : {"\r\nContent-Length:", "\r\ncontent-length:"}) {
        size_t at = req.find(h);
        if (at != std::string::npos && at < headerEnd)
            contentLength = std::strtoul(req.c_str() + at + std::strlen(h), nullptr, 10);
    }
    contentLength    = std::min<size_t>(contentLength, 65536);
    std::string body = req.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        body.append(buf, size_t(n));
    }

    const char* json = "application/json";
    if (path == "/" || path == "/index.html") {
        std::string page(reinterpret_cast<const char*>(ui_html), sizeof(ui_html));
        const std::string marker = "/*HDRKET_STATE*/null";
        if (size_t at = page.find(marker); at != std::string::npos)
            page.replace(at, marker.size(), stateJson());
        respond(fd, 200, "text/html; charset=utf-8", page);
    } else if (path == "/api/state") {
        respond(fd, 200, json, stateJson());
    } else if (method != "POST") {
        respond(fd, path.rfind("/api/", 0) == 0 ? 405 : 404, "text/plain", "no\n");
    } else if (path == "/api/settings") {
        applyForm(body);
        respond(fd, 200, json, stateJson());
    } else if (path == "/api/reset") {
        Settings defaults;
        Settings now           = currentSettings();
        defaults.peakNits     = now.peakNits; // the display didn't change
        defaults.peakOverride = now.peakOverride;
        updateSettings(defaults);
        respond(fd, 200, json, stateJson());
    } else if (path == "/api/save") {
        std::string where;
        bool        ok = savePreset(&where);
        respond(fd, ok ? 200 : 500, json,
                std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"path\":\"" + jsonEscape(where) + "\"}");
    } else {
        respond(fd, 404, "text/plain", "not found\n");
    }
}

void logAddresses(int port)
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) != 0)
            HK_INFO("tuning UI: http://%s:%d", ip, port);
    }
    freeifaddrs(list);
}

void serve()
{
    const char* bindEnv = std::getenv("HDRKET_BIND");
    const char* portEnv = std::getenv("HDRKET_PORT");
    std::string bindAddr = bindEnv && *bindEnv ? bindEnv : "0.0.0.0";
    int         basePort = portEnv && *portEnv ? std::atoi(portEnv) : 7331;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        HK_ERR("HDRKET_BIND: bad address %s", bindAddr.c_str());
        close(fd);
        return;
    }
    // Several games (or a launcher plus the game) may each run a server.
    int port = 0;
    for (int p = basePort; p < basePort + 10; p++) {
        addr.sin_port = htons(uint16_t(p));
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            port = p;
            break;
        }
    }
    if (!port || listen(fd, 8) != 0) {
        HK_WARN("tuning UI: no free port in %d-%d", basePort, basePort + 9);
        close(fd);
        return;
    }
    HK_INFO("tuning UI: http://localhost:%d", port);
    if (bindAddr == "0.0.0.0")
        logAddresses(port);

    for (;;) {
        int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            HK_WARN("tuning UI stopped: %s", std::strerror(errno));
            break;
        }
        handle(client);
        close(client);
    }
    close(fd);
}

} // namespace

void startTuningServer()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char* env = std::getenv("HDRKET_UI");
        if (env && !std::strcmp(env, "0"))
            return;
        // Detached for the life of the process; it only touches its own
        // socket and the settings store, both of which outlive it.
        std::thread(serve).detach();
    });
}

} // namespace hdrket
