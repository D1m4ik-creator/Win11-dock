#include "Common.h"

namespace dock {

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

std::wstring JoinPath(const std::wstring& dir, const std::wstring& file) {
    if (dir.empty()) return file;
    const wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + file;
    return dir + L"\\" + file;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (len == 0) return L".";
    std::wstring path(buf, len);
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos);
}

std::wstring ToLowerAscii(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

std::wstring BytesToString(double bytes) {
    if (bytes < 0.0) bytes = 0.0;
    static const wchar_t* kUnits[] = {L"Б", L"КБ", L"МБ", L"ГБ", L"ТБ"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }

    wchar_t buf[64]{};
    if (unit == 0 || bytes >= 100.0) {
        swprintf(buf, ARRAYSIZE(buf), L"%.0f %s", bytes, kUnits[unit]);
    } else if (bytes >= 10.0) {
        swprintf(buf, ARRAYSIZE(buf), L"%.1f %s", bytes, kUnits[unit]);
    } else {
        swprintf(buf, ARRAYSIZE(buf), L"%.2f %s", bytes, kUnits[unit]);
    }
    return buf;
}

// Network rates always in KB/s, so two readings days apart stay comparable and
// the number does not jump between units every sample.
std::wstring FormatKbPerSec(double bytesPerSec) {
    const double kb = bytesPerSec > 0.0 ? bytesPerSec / 1024.0 : 0.0;
    wchar_t buf[64]{};
    if (kb >= 1000.0) {
        swprintf(buf, ARRAYSIZE(buf), L"%.0f КБ/с", kb);
    } else if (kb >= 10.0) {
        swprintf(buf, ARRAYSIZE(buf), L"%.1f КБ/с", kb);
    } else {
        swprintf(buf, ARRAYSIZE(buf), L"%.2f КБ/с", kb);
    }
    return buf;
}

std::wstring FormatPercent(float percent) {
    wchar_t buf[32]{};
    swprintf(buf, ARRAYSIZE(buf), L"%.0f%%", ClampF(percent, 0.0f, 100.0f));
    return buf;
}

std::wstring FormatGhz(float ghz) {
    wchar_t buf[32]{};
    swprintf(buf, ARRAYSIZE(buf), L"%.2f", ghz);
    return buf;
}

std::wstring FormatGb(float gb) {
    wchar_t buf[32]{};
    swprintf(buf, ARRAYSIZE(buf), gb >= 10.0f ? L"%.1f ГБ" : L"%.2f ГБ", gb);
    return buf;
}

DWORD ReadRegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD result = fallback;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
        result = value;
    }
    RegCloseKey(key);
    return result;
}

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                           const std::wstring& fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
    wchar_t buf[512]{};
    DWORD type = 0;
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    std::wstring result = fallback;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = buf;
    }
    RegCloseKey(key);
    return result;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

namespace {

D2D1_COLOR_F SystemAccent() {
    // HKCU\...\DWM\AccentColor is stored as 0xAABBGGRR.
    const DWORD abgr = ReadRegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                                    L"AccentColor", 0);
    if (abgr != 0) {
        const uint32_t rr = abgr & 0xFF;
        const uint32_t gg = (abgr >> 8) & 0xFF;
        const uint32_t bb = (abgr >> 16) & 0xFF;
        // Pure black or white means the user never picked an accent colour;
        // using it would paint the chart and hover states invisible.
        const bool neutral = (rr == gg && gg == bb && (rr < 0x20 || rr > 0xE0));
        if (!neutral) {
            return D2D1::ColorF(static_cast<float>(rr) / 255.0f,
                                static_cast<float>(gg) / 255.0f,
                                static_cast<float>(bb) / 255.0f, 1.0f);
        }
    }
    // Windows 11 default accent.
    return ColorHex(0x4CC2FF);
}

bool SystemUsesLightTheme() {
    return ReadRegDword(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", 0) != 0;
}

}  // namespace

Theme LoadTheme() {
    Theme t;
    t.light = SystemUsesLightTheme();
    t.accent = SystemAccent();

    if (t.light) {
        t.text = ColorHex(0x1B1B1B);
        t.textMuted = ColorHex(0x5C5C5C);
        t.pillBg = ColorHex(0xFFFFFF, 0.86f);
        t.pillBorder = ColorHex(0x000000, 0.10f);
        t.cardBg = ColorHex(0xFAFAFA);
        t.cardBorder = ColorHex(0x000000, 0.12f);
        t.track = ColorHex(0x000000, 0.20f);
        t.hoverBg = ColorHex(0x000000, 0.08f);
        t.pressedBg = ColorHex(0x000000, 0.14f);
        t.shadow = ColorHex(0x000000, 0.20f);
        t.buttonBorder = ColorHex(0x000000, 0.14f);
    } else {
        t.text = ColorHex(0xF3F3F3);
        t.textMuted = ColorHex(0xA6A6A6);
        t.pillBg = ColorHex(0x1A1A1A, 0.86f);
        t.pillBorder = ColorHex(0xFFFFFF, 0.09f);
        t.cardBg = ColorHex(0x191919);
        t.cardBorder = ColorHex(0xFFFFFF, 0.10f);
        t.track = ColorHex(0xFFFFFF, 0.26f);
        t.hoverBg = ColorHex(0xFFFFFF, 0.10f);
        t.pressedBg = ColorHex(0xFFFFFF, 0.17f);
        t.shadow = ColorHex(0x000000, 0.60f);
        t.buttonBorder = ColorHex(0xFFFFFF, 0.13f);
    }
    return t;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Config Config::Load(const std::wstring& baseDir) {
    Config c;
    const std::wstring ini = JoinPath(baseDir, L"win11-dock.ini");

    c.magnify = GetPrivateProfileIntW(L"dock", L"Magnify", 1, ini.c_str()) != 0;
    c.enableHotkey = GetPrivateProfileIntW(L"dock", L"Hotkey", 1, ini.c_str()) != 0;
    c.hideWhenFullscreen =
        GetPrivateProfileIntW(L"dock", L"HideWhenFullscreen", 1, ini.c_str()) != 0;
    c.maxWindows = static_cast<int>(GetPrivateProfileIntW(L"dock", L"MaxWindows", 8, ini.c_str()));
    c.maxWindows = std::clamp(c.maxWindows, 1, 20);

    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"dock", L"CardOpacity", L"0.98", buf, ARRAYSIZE(buf), ini.c_str());
    c.cardOpacity = ClampF(static_cast<float>(_wtof(buf)), 0.75f, 1.0f);

    const int mode = static_cast<int>(GetPrivateProfileIntW(L"dock", L"Theme", 0, ini.c_str()));
    c.themeMode = mode == 1 ? ThemeMode::Light : (mode == 2 ? ThemeMode::Dark : ThemeMode::Auto);

    GetPrivateProfileStringW(L"dock", L"AccentColor", L"", buf, ARRAYSIZE(buf), ini.c_str());
    if (buf[0] != 0) c.accentOverride = static_cast<uint32_t>(wcstoul(buf, nullptr, 16) & 0xFFFFFF);

    return c;
}

void Config::Save(const std::wstring& baseDir) const {
    const std::wstring ini = JoinPath(baseDir, L"win11-dock.ini");
    auto put = [&](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"dock", key, value.c_str(), ini.c_str());
    };
    put(L"Magnify", magnify ? L"1" : L"0");
    put(L"Hotkey", enableHotkey ? L"1" : L"0");
    put(L"HideWhenFullscreen", hideWhenFullscreen ? L"1" : L"0");
    put(L"MaxWindows", std::to_wstring(maxWindows));

    wchar_t num[64]{};
    swprintf(num, ARRAYSIZE(num), L"%.3f", cardOpacity);
    put(L"CardOpacity", num);

    put(L"Theme", std::to_wstring(static_cast<int>(themeMode)));

    if (accentOverride != 0) {
        wchar_t hex[16]{};
        swprintf(hex, ARRAYSIZE(hex), L"%06X", accentOverride);
        put(L"AccentColor", hex);
    }
}

// ---------------------------------------------------------------------------
// Shell geometry
// ---------------------------------------------------------------------------

namespace {

// Depth-first search for a descendant of `parent` with the given class name.
// Taskbar internals nest differently across Windows builds: MSTaskListWClass
// sits directly under ReBarWindow32 on some builds and under MSTaskSwWClass on
// others, so a fixed chain breaks on every feature update.
HWND FindDescendantByClass(HWND parent, const wchar_t* cls, int depth = 0) {
    if (!parent || depth > 6) return nullptr;
    for (HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr); child;
         child = FindWindowExW(parent, child, nullptr, nullptr)) {
        wchar_t name[256]{};
        if (GetClassNameW(child, name, ARRAYSIZE(name)) > 0) {
            if (lstrcmpiW(name, cls) == 0) return child;
        }
        if (HWND found = FindDescendantByClass(child, cls, depth + 1)) return found;
    }
    return nullptr;
}

}  // namespace

ShellLayout QueryShellLayout() {
    ShellLayout out;

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        out.taskbarVisible = false;
        return out;
    }

    RECT trayRect{};
    if (!GetWindowRect(tray, &trayRect)) {
        out.taskbarVisible = false;
        return out;
    }

    out.taskbar = trayRect;
    out.taskbarVisible = IsWindowVisible(tray) != 0 && (trayRect.bottom - trayRect.top) > 4;

    const HMONITOR monitor = MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return out;

    out.monitor = mi.rcMonitor;

    HDC dc = GetDC(nullptr);
    out.dpi = dc ? static_cast<float>(GetDeviceCaps(dc, LOGPIXELSX)) : 96.0f;
    if (dc) ReleaseDC(nullptr, dc);
    if (out.dpi <= 0.0f) out.dpi = 96.0f;

    // Tray cluster (clock, tray icons, show-desktop strip).
    RECT notify{};
    HWND notifyWnd = FindDescendantByClass(tray, L"TrayNotifyWnd");
    if (notifyWnd) GetWindowRect(notifyWnd, &notify);

    out.freeRight = (notify.right > notify.left) ? notify.left : trayRect.right;

    // Task list; when no window is running it has zero width.
    RECT taskList{};
    HWND listWnd = FindDescendantByClass(tray, L"MSTaskListWClass");
    if (listWnd) GetWindowRect(listWnd, &taskList);

    out.freeLeft = (taskList.right > taskList.left) ? taskList.right : trayRect.left;

    out.valid = true;
    return out;
}

bool IsForegroundFullscreen(HWND ownWindow) {
    const HWND fg = GetForegroundWindow();
    if (!fg || fg == ownWindow) return false;

    wchar_t cls[256]{};
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    if (lstrcmpiW(cls, L"Progman") == 0 || lstrcmpiW(cls, L"WorkerW") == 0) return false;
    if (lstrcmpiW(cls, L"Shell_TrayWnd") == 0) return false;

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;

    const HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return false;

    const RECT& m = mi.rcMonitor;
    const int tol = 2;
    return wr.left <= m.left + tol && wr.top <= m.top + tol && wr.right >= m.right - tol &&
           wr.bottom >= m.bottom - tol;
}

}  // namespace dock
