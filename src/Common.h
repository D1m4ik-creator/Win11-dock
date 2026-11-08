#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// winsock2.h must come before windows.h
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>

#include <powerbase.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace dock {

// ---------------------------------------------------------------------------
// Math / color
// ---------------------------------------------------------------------------

inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Frame-rate independent exponential smoothing toward a target.
inline float Approach(float current, float target, float rate, float dtSeconds) {
    const float t = 1.0f - std::pow(1.0f - ClampF(rate, 0.0f, 1.0f), dtSeconds * 60.0f);
    return Lerp(current, target, t);
}

inline D2D1_COLOR_F ColorHex(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, alpha);
}

inline D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float alpha) {
    return D2D1::ColorF(c.r, c.g, c.b, ClampF(alpha, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// Strings / paths
// ---------------------------------------------------------------------------

std::wstring JoinPath(const std::wstring& dir, const std::wstring& file);
std::wstring ExeDir();
std::wstring ToLowerAscii(const std::wstring& s);
std::wstring BytesToString(double bytes);
std::wstring FormatKbPerSec(double bytesPerSec);
std::wstring FormatPercent(float percent);
std::wstring FormatGhz(float ghz);
std::wstring FormatGb(float gb);

DWORD ReadRegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD fallback);
std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                           const std::wstring& fallback);

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

struct Theme {
    bool light = false;

    D2D1_COLOR_F text{};
    D2D1_COLOR_F textMuted{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F pillBg{};
    D2D1_COLOR_F pillBorder{};
    D2D1_COLOR_F cardBg{};
    D2D1_COLOR_F cardBorder{};
    D2D1_COLOR_F track{};
    D2D1_COLOR_F hoverBg{};
    D2D1_COLOR_F pressedBg{};
    D2D1_COLOR_F shadow{};
    D2D1_COLOR_F buttonBorder{};

    D2D1_COLOR_F Separator() const { return WithAlpha(text, light ? 0.16f : 0.18f); }
};

Theme LoadTheme();

// ---------------------------------------------------------------------------
// Config (INI next to the exe)
// ---------------------------------------------------------------------------

enum class ThemeMode { Auto = 0, Light = 1, Dark = 2 };

struct Config {
    bool magnify = true;
    bool enableHotkey = true;
    bool hideWhenFullscreen = true;
    int maxWindows = 8;
    float cardOpacity = 0.98f;
    ThemeMode themeMode = ThemeMode::Auto;
    uint32_t accentOverride = 0;

    static Config Load(const std::wstring& baseDir);
    void Save(const std::wstring& baseDir) const;
};

// ---------------------------------------------------------------------------
// Shell geometry
// ---------------------------------------------------------------------------

struct ShellLayout {
    bool valid = false;
    bool taskbarVisible = true;
    RECT taskbar{};    // screen px
    RECT monitor{};    // screen px
    float dpi = 96.0f;
    int freeLeft = 0;  // px: right edge of the task list
    int freeRight = 0; // px: left edge of the tray cluster
};

ShellLayout QueryShellLayout();

bool IsForegroundFullscreen(HWND ownWindow);

}  // namespace dock
