#pragma once

#include "Common.h"

namespace dock {

// One icon in the strip: either an app window group or a system metric.
struct AppEntry {
    DWORD pid = 0;
    std::wstring exePath;
    std::wstring title;                 // title of the currently shown window
    std::vector<HWND> windows;          // top-level windows of this process
    size_t activeIndex = 0;             // which window is shown/activated
    bool foreground = false;

    HWND CurrentWindow() const {
        return activeIndex < windows.size() ? windows[activeIndex] : nullptr;
    }
};

class WindowList {
public:
    void Refresh();

    const std::vector<AppEntry>& Apps() const { return apps_; }

    // Brings the entry's current window to the foreground, and cycles to the
    // next window of the same process if it is already in the foreground.
    static void Activate(AppEntry& entry);
    static void CloseWindow(HWND hwnd);

    // The shell taskbar window for the monitor that owns `hwnd`, or the primary.
    static HWND TaskbarForWindow(HWND hwnd);
    static RECT TaskbarRect(HWND taskbar);

    static bool IsFullscreenForeground(HWND ownWindow);

private:
    std::vector<AppEntry> apps_;
};

// Cached app icons rendered from the executable image.
//
// Bitmaps belong to the D2D device context that created them: after a device
// loss the owner must call Clear() and re-create the cache.
class IconCache {
public:
    ~IconCache();
    bool Init(ID2D1DeviceContext* ctx);
    // Returns a bitmap for the executable, or nullptr when unavailable.
    ID2D1Bitmap* Get(const std::wstring& exePath);
    // Same, but falls back to the live window icon when the exe has none.
    ID2D1Bitmap* GetForWindow(const std::wstring& exePath, HWND hwnd);
    void SetContext(ID2D1DeviceContext* ctx) { ctx_ = ctx; }
    void ReplaceContext(ID2D1DeviceContext* ctx);
    void Clear();

private:
    ID2D1Bitmap* Load(const std::wstring& exePath);

    ID2D1DeviceContext* ctx_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    std::unordered_map<std::wstring, ID2D1Bitmap*> cache_;
};

// Small vector glyphs drawn with geometry so no font/icon file is needed.
enum class Glyph { Cpu, Memory, NetworkIn, NetworkOut, ChevronUp, Gear, Close, Up, Down, Apps, Bolt };
void DrawGlyph(ID2D1DeviceContext* ctx, Glyph glyph, const D2D1_RECT_F& box,
               const D2D1_COLOR_F& color, float strokeWidth = 1.4f);

}  // namespace dock
