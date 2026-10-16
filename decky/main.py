# SPDX-License-Identifier: MIT
#
# HDR Ket Decky backend.
#
# - Installs the bundled Vulkan layer into the user's ~/.local (it only
#   activates for games launched with ENABLE_HDRKET=1).
# - Reads the display's identity and peak from its EDID, and keeps the
#   per-display default peak (~/.config/hdrket/displays/<display>.conf).
# - Reads and edits per-game profiles (~/.config/hdrket/presets/steam-<appid>.conf).
#   While the game runs, edits go to the layer's own HTTP API so they apply on
#   the next frame and the layer saves them; otherwise the file is edited.
#
# File formats and the display key must match src/params.cpp in the layer.

import asyncio
import hashlib
import json
import os
import re
import shutil
import socket
import urllib.parse
import urllib.request
import zlib

import decky

HOME = decky.DECKY_USER_HOME
CONFIG_DIR = os.path.join(HOME, ".config", "hdrket")
PRESETS_DIR = os.path.join(CONFIG_DIR, "presets")
DISPLAYS_DIR = os.path.join(CONFIG_DIR, "displays")

LAYER_DIR = os.path.join(HOME, ".local", "share", "hdrket")
LAYER_LIB = os.path.join(LAYER_DIR, "libhdrket.so")
MANIFEST = os.path.join(HOME, ".local", "share", "vulkan", "implicit_layer.d", "hdrket.json")
BUNDLED_LIB = os.path.join(decky.DECKY_PLUGIN_DIR, "bin", "libhdrket.so")
BUNDLED_MANIFEST = os.path.join(decky.DECKY_PLUGIN_DIR, "bin", "hdrket.json.in")

PORTS = range(7331, 7341)

# Mirrors Settings in the layer's params.hpp. Used only when editing a profile
# while its game is not running.
DEFAULTS = {
    "middle_grey_nits": 36.0,
    "expansion": 0.5,
    "saturation": 1.0,
    "black_floor": 0.0,
    "eotf": "gamma22",
    "tonemap": "adaptive",
    "gamut_expansion": 0.0,
    "adapt_release": 2.0,
    "adapt_strength": 0.5,
    "shoulder_start": 0.5,
    "shoulder_pow": 2.5,
}
DEFAULT_PEAK = 1000.0
STRING_KEYS = {"eotf", "tonemap"}
# Comparison aids only make sense live; they are never saved.
LIVE_ONLY_KEYS = {"split", "pattern", "bypass"}


# ---------------------------------------------------------------------------
# Display identity (EDID), matching parseEdid() in params.cpp


def _sanitize(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)


def _parse_edid(edid: bytes) -> dict:
    def byte(i):
        return edid[i] if i < len(edid) else 0

    name = ""
    for d in range(54, 126, 18):
        if byte(d) == 0 and byte(d + 1) == 0 and byte(d + 3) == 0xFC:
            raw = bytes(byte(i) for i in range(d + 5, d + 18))
            name = raw.split(b"\x0a")[0].decode("ascii", "replace").strip()
    if not name:
        mfg = (byte(8) << 8) | byte(9)
        name = "".join(chr(ord("@") + ((mfg >> s) & 31)) for s in (10, 5, 0))

    peak = 0.0
    for b in range(128, len(edid) - 127, 128):
        if byte(b) != 0x02:
            continue
        dtd = byte(b + 2)
        i = b + 4
        while i < b + dtd and i < b + 128:
            tag, length = byte(i) >> 5, byte(i) & 31
            if tag == 7 and length >= 4 and byte(i + 1) == 6 and byte(i + 4) != 0:
                peak = 50.0 * 2 ** (byte(i + 4) / 32.0)
            i += 1 + length

    key = f"{_sanitize(name)}-{zlib.crc32(edid) & 0xFFFFFFFF:08x}"
    return {"name": name, "key": key, "edid_peak": round(peak)}


def _read(path: str, mode: str = "r"):
    try:
        with open(path, mode) as f:
            return f.read()
    except OSError:
        return b"" if "b" in mode else ""


def detect_display() -> dict:
    base = "/sys/class/drm"
    try:
        connectors = sorted(os.listdir(base))
    except OSError:
        connectors = []
    found = []
    for c in connectors:
        path = os.path.join(base, c)
        if _read(os.path.join(path, "status")).strip() != "connected":
            continue
        if _read(os.path.join(path, "enabled")).strip() != "enabled":
            continue
        edid = _read(os.path.join(path, "edid"), "rb")
        if len(edid) >= 128:
            found.append(_parse_edid(edid))
    if not found:
        return {"name": "Unknown display", "key": "unknown-display", "edid_peak": 0, "count": 0}
    return {**found[0], "count": len(found)}


# ---------------------------------------------------------------------------
# key = value files


def _read_kv(path: str) -> list:
    pairs = []
    for line in _read(path).splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        pairs.append((key, value))
    return pairs


def _write_atomic(path: str, text: str):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def _format_value(key: str, value) -> str:
    if key in STRING_KEYS:
        return f'"{value}"'
    return f"{float(value):g}"


def display_default_peak(display: dict) -> tuple:
    """(peak, configured): the saved profile, else the EDID value, else 1000."""
    for key, value in _read_kv(os.path.join(DISPLAYS_DIR, display["key"] + ".conf")):
        if key == "peak_nits":
            try:
                return float(value), True
            except ValueError:
                pass
    return float(display["edid_peak"] or DEFAULT_PEAK), False


def save_display_peak(display: dict, nits: float):
    _write_atomic(os.path.join(DISPLAYS_DIR, display["key"] + ".conf"),
                  f"# HDR Ket display profile: {display['name']}\npeak_nits = {nits:g}\n")


def preset_path(appid: int) -> str:
    return os.path.join(PRESETS_DIR, f"steam-{appid}.conf")


def read_profile(appid: int, display: dict) -> dict:
    """A game's saved settings as the layer would load them on this display."""
    settings = dict(DEFAULTS)
    default_peak, _ = display_default_peak(display)
    settings["peak_nits"] = default_peak
    settings["peak_override"] = False
    mine = "peak_nits@" + display["key"]
    any_display = None
    for key, value in _read_kv(preset_path(appid)):
        if key == mine:
            settings["peak_nits"] = float(value)
            settings["peak_override"] = True
        elif key == "peak_nits":
            any_display = value
        elif key in DEFAULTS:
            settings[key] = value if key in STRING_KEYS else float(value)
    if any_display is not None and not settings["peak_override"]:
        settings["peak_nits"] = float(any_display)
        settings["peak_override"] = True
    settings["middle_grey_nits"] = float(settings["middle_grey_nits"])
    settings["paper_white_nits"] = round(settings["middle_grey_nits"] / 0.18)
    return settings


def write_profile(appid: int, display: dict, settings: dict, reset: bool = False):
    """Writes the game's profile, keeping peak overrides for other displays."""
    mine = "peak_nits@" + display["key"]
    others = [(k, v) for k, v in _read_kv(preset_path(appid))
              if k.startswith("peak_nits@") and k != mine]
    lines = [f"# HDR Ket preset for steam-{appid}",
             "# Peak brightness comes from ../displays/ unless overridden per display below"]
    if not reset:
        lines += [f"{k} = {_format_value(k, settings[k])}" for k in DEFAULTS]
    lines += [f"{k} = {v}" for k, v in others]
    if not reset and settings.get("peak_override"):
        lines.append(f"{mine} = {float(settings['peak_nits']):g}  # {display['name']}")
    _write_atomic(preset_path(appid), "\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# Live games: the layer's HTTP API


def _http(port: int, path: str, data: dict = None, timeout: float = 0.4):
    url = f"http://127.0.0.1:{port}{path}"
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    with urllib.request.urlopen(url, data=body, timeout=timeout) as r:
        return json.loads(r.read().decode())


def find_instances() -> list:
    """(port, state) for every running game with HDR Ket active."""
    found = []
    for port in PORTS:
        try:
            found.append((port, _http(port, "/api/state", timeout=0.15)))
        except Exception:
            continue
    return found


def lan_address() -> str:
    # Connecting a UDP socket sends nothing; it just picks the outgoing interface.
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            return s.getsockname()[0]
    except OSError:
        return "localhost"


# ---------------------------------------------------------------------------
# Layer installation


def _sha256(path: str) -> str:
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return ""


def layer_status() -> dict:
    try:
        manifest = json.loads(_read(MANIFEST) or "{}")
        library = manifest.get("layer", {}).get("library_path", "")
    except ValueError:
        library = ""
    if not library:
        return {"state": "missing", "path": ""}
    if os.path.realpath(library) != os.path.realpath(LAYER_LIB):
        # Someone installed a build by hand (scripts/dev-install.sh): leave it.
        return {"state": "developer", "path": library}
    return {"state": "installed" if os.path.exists(LAYER_LIB) else "missing", "path": library}


def install_layer() -> dict:
    status = layer_status()
    if status["state"] == "developer":
        decky.logger.info(f"developer build in use, not touching it: {status['path']}")
        return status
    if not os.path.exists(BUNDLED_LIB):
        decky.logger.error("plugin has no bundled layer (bin/libhdrket.so)")
        return status
    if status["state"] == "installed" and _sha256(BUNDLED_LIB) == _sha256(LAYER_LIB):
        return status
    os.makedirs(LAYER_DIR, exist_ok=True)
    shutil.copyfile(BUNDLED_LIB, LAYER_LIB + ".tmp")
    os.replace(LAYER_LIB + ".tmp", LAYER_LIB)
    _write_atomic(MANIFEST, _read(BUNDLED_MANIFEST).replace("@LIB_PATH@", LAYER_LIB))
    decky.logger.info(f"installed layer to {LAYER_LIB}")
    return layer_status()


# ---------------------------------------------------------------------------


class Plugin:
    async def _main(self):
        await asyncio.to_thread(install_layer)

    async def _unload(self):
        pass

    async def _uninstall(self):
        # Remove the layer we installed; profiles stay in ~/.config/hdrket.
        if layer_status()["state"] == "installed":
            for path in (MANIFEST, LAYER_LIB):
                try:
                    os.remove(path)
                except OSError:
                    pass

    async def get_overview(self, appid: int) -> dict:
        """Everything the panel shows for one game (appid 0 = none)."""
        return await asyncio.to_thread(self._overview, int(appid or 0))

    def _overview(self, appid: int) -> dict:
        display = detect_display()
        default_peak, configured = display_default_peak(display)
        result = {
            "layer": layer_status(),
            "display": {**display, "default_peak": default_peak, "configured": configured},
            "game": None,
        }
        if not appid:
            return result
        live = next(((p, s) for p, s in find_instances() if s.get("appid") == appid), None)
        if live:
            port, state = live
            result["game"] = {"appid": appid, "live": True, "port": port,
                              "url": f"http://{lan_address()}:{port}",
                              "settings": state["settings"], "stats": state["stats"],
                              "output": state["output"]}
        else:
            result["game"] = {"appid": appid, "live": False,
                              "has_profile": os.path.exists(preset_path(appid)),
                              "settings": read_profile(appid, display)}
        return result

    async def set_settings(self, appid: int, changes: dict) -> dict:
        return await asyncio.to_thread(self._set_settings, int(appid), changes)

    def _set_settings(self, appid: int, changes: dict) -> dict:
        changes = {k: v for k, v in changes.items() if v is not None}
        live = next((p for p, s in find_instances() if s.get("appid") == appid), None)
        if live:
            form = {k: (1 if v is True else 0 if v is False else v) for k, v in changes.items()}
            _http(live, "/api/settings", form, timeout=1.0)
        else:
            display = detect_display()
            settings = read_profile(appid, display)
            for key, value in changes.items():
                if key in LIVE_ONLY_KEYS:
                    continue
                if key == "peak_override" and not value:
                    settings["peak_nits"] = display_default_peak(display)[0]
                if key in settings:
                    settings[key] = value
            write_profile(appid, display, settings)
        return self._overview(appid)

    async def reset_game(self, appid: int) -> dict:
        return await asyncio.to_thread(self._reset_game, int(appid))

    def _reset_game(self, appid: int) -> dict:
        live = next((p for p, s in find_instances() if s.get("appid") == appid), None)
        if live:
            _http(live, "/api/reset", {}, timeout=1.0)
        elif os.path.exists(preset_path(appid)):
            write_profile(appid, detect_display(), {}, reset=True)
        return self._overview(appid)

    async def set_default_peak(self, nits: float, appid: int) -> dict:
        return await asyncio.to_thread(self._set_default_peak, float(nits), int(appid or 0))

    def _set_default_peak(self, nits: float, appid: int) -> dict:
        nits = min(max(nits, 80.0), 10000.0)
        display = detect_display()
        save_display_peak(display, nits)
        # Running games pick it up at once (unless they have their own peak).
        for port, _ in find_instances():
            try:
                _http(port, "/api/display_peak", {"nits": nits}, timeout=1.0)
            except Exception as e:
                decky.logger.warning(f"could not update game on port {port}: {e}")
        return self._overview(appid)
