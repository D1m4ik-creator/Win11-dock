#include "WindowList.h"

#include "Surface.h"

#include <dwmapi.h>
#include <propkey.h>

namespace dock {

namespace {

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buf[256]{};
    GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return buf;
}

std::wstring ProcessPathOf(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD size = ARRAYSIZE(buf);
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size)) result.assign(buf, size);
    CloseHandle(proc);
    return result;
}

// UWP apps run inside ApplicationFrameHost; the real app is the child
// CoreWindow process. Returns the hosting pid/exe when applicable.
bool ResolveFrameHostApp(HWND hwnd, DWORD& pid, std::wstring& exe) {
    const std::wstring cls = ClassNameOf(hwnd);
    if (cls != L"ApplicationFrameWindow") return false;

    struct Ctx {
        DWORD pid = 0;
    } ctx;

    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t buf[256]{};
            GetClassNameW(child, buf, ARRAYSIZE(buf));
            if (lstrcmpiW(buf, L"Windows.UI.Core.CoreWindow") == 0) {
                auto* c = reinterpret_cast<Ctx*>(param);
                DWORD childPid = 0;
                GetWindowThreadProcessId(child, &childPid);
                if (childPid != 0 && childPid != GetCurrentProcessId()) {
                    c->pid = childPid;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.pid == 0) return false;
    pid = ctx.pid;
    exe = ProcessPathOf(ctx.pid);
    return !exe.empty();
}

bool IsTaskbarWindow(HWND hwnd) {
    static const wchar_t* kSkipClasses[] = {
        L"Progman",          L"WorkerW",           L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow",
        L"ApplicationManager_DesktopShellWindow", L"Shell_InputSwitchTopLevelWindow",
        L"ForegroundStaging", L"MultitaskingViewFrame", L"XamlExplorerHostIslandWindow",
        L"Windows.Internal.Shell.TabProxyWindow", L"TaskListThumbnailWnd",
        L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
    };
    const std::wstring cls = ClassNameOf(hwnd);
    for (const wchar_t* skip : kSkipClasses) {
        if (lstrcmpiW(cls.c_str(), skip) == 0) return true;
    }
    return false;
}

bool IsCandidateWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;

    // Windows that are only owned popups of another app are skipped: keep them
    // only if they are proper top-level app windows.
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    if (IsCloaked(hwnd)) return false;
    if (IsTaskbarWindow(hwnd)) return false;

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    if (r.right - r.left < 32 || r.bottom - r.top < 32) return false;

    return true;
}

void ForceForeground(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
    }

    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
    const DWORD thisThread = GetCurrentThreadId();
    const bool attached =
        foregroundThread != 0 && foregroundThread != thisThread &&
        AttachThreadInput(thisThread, foregroundThread, TRUE) != 0;

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (attached) AttachThreadInput(thisThread, foregroundThread, FALSE);

    if (GetForegroundWindow() != hwnd) {
        // Last resort: a brief ALT tap releases the foreground lock.
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(hwnd);
    }
}

}  // namespace

void WindowList::Refresh() {
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundPid);

    const DWORD selfPid = GetCurrentProcessId();

    std::vector<AppEntry> apps;
    std::unordered_map<DWORD, size_t> indexOf;

    // Enumeration order of GW_HWNDNEXT follows Z-order, so this yields an
    // MRU-ish ordering without extra bookkeeping.
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if (!IsCandidateWindow(hwnd)) continue;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == selfPid) continue;

        std::wstring exe = ProcessPathOf(pid);
        if (exe.empty()) continue;

        DWORD appPid = pid;
        std::wstring appExe = exe;
        if (ResolveFrameHostApp(hwnd, appPid, appExe)) {
            // Grouped under the hosted app process, but the window handle stays
            // the frame window (that is what gets activated).
        }

        const std::wstring keyExe = ToLowerAscii(appExe);
        auto it = indexOf.find(appPid);
        if (it == indexOf.end()) {
            AppEntry entry;
            entry.pid = appPid;
            entry.exePath = appExe;
            entry.windows.push_back(hwnd);
            entry.foreground = (pid == foregroundPid || appPid == foregroundPid);
            entry.title.clear();
            indexOf[appPid] = apps.size();
            apps.push_back(std::move(entry));
        } else {
            AppEntry& entry = apps[it->second];
            entry.windows.push_back(hwnd);
            if (pid == foregroundPid || appPid == foregroundPid) entry.foreground = true;
        }
        (void)keyExe;
    }

    // Make sure a just-opened app that has no visible window yet does not break
    // ordering; then resolve titles for the visible window of each entry.
    const size_t limit = 24;
    if (apps.size() > limit) apps.resize(limit);

    for (AppEntry& entry : apps) {
        // Prefer the foreground window as the displayed one.
        for (size_t i = 0; i < entry.windows.size(); ++i) {
            if (entry.windows[i] == foreground) {
                entry.activeIndex = i;
                break;
            }
        }
        if (entry.activeIndex >= entry.windows.size()) entry.activeIndex = 0;

        HWND hwnd = entry.CurrentWindow();
        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        entry.title = title;
    }

    apps_ = std::move(apps);
}

void WindowList::Activate(AppEntry& entry) {
    if (entry.windows.empty()) return;

    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr && foreground == entry.CurrentWindow()) {
        // Already focused: cycle to the next window of the same app.
        if (entry.windows.size() > 1) {
            entry.activeIndex = (entry.activeIndex + 1) % entry.windows.size();
        }
    }

    HWND target = entry.CurrentWindow();
    if (!target) return;
    ForceForeground(target);

    if (GetForegroundWindow() != target && !entry.exePath.empty()) {
        ShellExecuteW(nullptr, L"open", entry.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void WindowList::CloseWindow(HWND hwnd) {
    if (!hwnd) return;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

HWND WindowList::TaskbarForWindow(HWND hwnd) {
    const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hwnd) return primary;

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!primary) return nullptr;
    if (MonitorFromWindow(primary, MONITOR_DEFAULTTOPRIMARY) == monitor) return primary;

    struct Ctx {
        HMONITOR monitor = nullptr;
        HWND found = nullptr;
    } ctx{monitor, nullptr};

    EnumWindows(
        [](HWND w, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (ClassNameOf(w) != L"Shell_SecondaryTrayWnd") return TRUE;
            if (MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY) == c->monitor) {
                c->found = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return ctx.found ? ctx.found : primary;
}

RECT WindowList::TaskbarRect(HWND taskbar) {
    RECT r{};
    if (taskbar) GetWindowRect(taskbar, &r);
    return r;
}

bool WindowList::IsFullscreenForeground(HWND ownWindow) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == ownWindow) return false;
    if (IsTaskbarWindow(foreground)) return false;

    RECT wr{};
    if (!GetWindowRect(foreground, &wr)) return false;

    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return false;

    const RECT& mr = mi.rcMonitor;
    return wr.left <= mr.left && wr.top <= mr.top && wr.right >= mr.right &&
           wr.bottom >= mr.bottom;
}

// ---------------------------------------------------------------------------
// IconCache
// ---------------------------------------------------------------------------

IconCache::~IconCache() {
    Clear();
    if (wic_) {
        wic_->Release();
        wic_ = nullptr;
    }
}

bool IconCache::Init(ID2D1DeviceContext* ctx) {
    ctx_ = ctx;
    if (!wic_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&wic_));
    }
    return wic_ != nullptr;
}

void IconCache::Clear() {
    for (auto& kv : cache_) {
        if (kv.second) kv.second->Release();
    }
    cache_.clear();
}

namespace {

// Converts a 32bpp HBITMAP into a premultiplied D2D bitmap.
ID2D1Bitmap* BitmapFromHBITMAP(ID2D1DeviceContext* ctx, HBITMAP hbm) {
    if (!ctx || !hbm) return nullptr;

    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm)) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const size_t stride = static_cast<size_t>(bm.bmWidth) * 4;
    std::vector<BYTE> pixels(stride * static_cast<size_t>(bm.bmHeight));

    HDC dc = CreateCompatibleDC(nullptr);
    const int lines = GetDIBits(dc, hbm, 0, static_cast<UINT>(bm.bmHeight), pixels.data(), &bi,
                                DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines == 0) return nullptr;

    // Shell-rendered icons come back as straight alpha; D2D wants premultiplied.
    bool anyAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    } else {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned a = pixels[i + 3];
            if (a == 255) continue;
            pixels[i + 0] = static_cast<BYTE>(pixels[i + 0] * a / 255);
            pixels[i + 1] = static_cast<BYTE>(pixels[i + 1] * a / 255);
            pixels[i + 2] = static_cast<BYTE>(pixels[i + 2] * a / 255);
        }
    }

    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);

    ID2D1Bitmap* bitmap = nullptr;
    ctx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(bm.bmWidth),
                                  static_cast<UINT32>(bm.bmHeight)),
                      pixels.data(), static_cast<UINT32>(stride), &props, &bitmap);
    return bitmap;
}

ID2D1Bitmap* IconFromShellImage(ID2D1DeviceContext* ctx, const std::wstring& path) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }

    HBITMAP hbm = nullptr;
    SIZE size{96, 96};
    HRESULT hr = factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
    factory->Release();
    if (FAILED(hr) || !hbm) return nullptr;

    ID2D1Bitmap* bitmap = BitmapFromHBITMAP(ctx, hbm);
    DeleteObject(hbm);
    return bitmap;
}

ID2D1Bitmap* IconFromWindow(ID2D1DeviceContext* ctx, HWND hwnd) {
    if (!hwnd) return nullptr;

    HICON icon = reinterpret_cast<HICON>(
        SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 120, nullptr));
    if (!icon) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(
            SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 120, nullptr));
    }
    if (!icon) return nullptr;

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return nullptr;
    ID2D1Bitmap* bitmap = BitmapFromHBITMAP(ctx, info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    return bitmap;
}

}  // namespace

ID2D1Bitmap* IconCache::Load(const std::wstring& exePath) {
    if (!ctx_ || exePath.empty()) return nullptr;
    if (!wic_) Init(ctx_);

    ID2D1Bitmap* bitmap = nullptr;
    if (exePath.find(L':') != std::wstring::npos) {
        bitmap = IconFromShellImage(ctx_, exePath);
    }
    if (!bitmap) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON)) {
            if (sfi.hIcon) {
                ICONINFO info{};
                if (GetIconInfo(sfi.hIcon, &info)) {
                    bitmap = BitmapFromHBITMAP(ctx_, info.hbmColor);
                    if (info.hbmMask) DeleteObject(info.hbmMask);
                    if (info.hbmColor) DeleteObject(info.hbmColor);
                }
                DestroyIcon(sfi.hIcon);
            }
        }
    }
    return bitmap;
}

ID2D1Bitmap* IconCache::Get(const std::wstring& exePath) {
    if (!ctx_ || exePath.empty()) return nullptr;

    const std::wstring key = ToLowerAscii(exePath);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    ID2D1Bitmap* bitmap = Load(exePath);
    cache_[key] = bitmap;
    return bitmap;
}

ID2D1Bitmap* IconCache::GetForWindow(const std::wstring& exePath, HWND hwnd) {
    if (!ctx_) return nullptr;

    const std::wstring key = ToLowerAscii(exePath) + L"#";
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second) return it->second;

    ID2D1Bitmap* bitmap = Load(exePath);
    if (!bitmap) bitmap = IconFromWindow(ctx_, hwnd);
    if (bitmap) cache_[key] = bitmap;
    return bitmap;
}

void IconCache::ReplaceContext(ID2D1DeviceContext* ctx) {}

// ---------------------------------------------------------------------------
// Glyphs
// ---------------------------------------------------------------------------

void DrawGlyph(ID2D1DeviceContext* ctx, Glyph glyph, const D2D1_RECT_F& box,
               const D2D1_COLOR_F& color, float strokeWidth) {
    ID2D1Factory* factory = nullptr;
    ctx->GetFactory(&factory);
    if (!factory) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(ctx->CreateSolidColorBrush(color, &brush))) return;

    const float w = box.right - box.left;
    const float h = box.bottom - box.top;
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float s = std::min(w, h);

    auto line = [&](float x1, float y1, float x2, float y2) {
        ctx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, strokeWidth);
    };

    switch (glyph) {
        case Glyph::Cpu: {
            const float pad = s * 0.22f;
            const D2D1_RECT_F chip = D2D1::RectF(box.left + pad, box.top + pad, box.right - pad,
                                                 box.bottom - pad);
            StrokeRoundRect(ctx, chip, s * 0.10f, color, strokeWidth);
            const float inner = s * 0.10f;
            ctx->FillRectangle(D2D1::RectF(chip.left + inner, chip.top + inner,
                                           chip.right - inner, chip.bottom - inner),
                               brush);
            for (int i = 0; i < 3; ++i) {
                const float t = (i + 1) / 4.0f;
                const float y = Lerp(box.top + pad * 0.35f, box.bottom - pad * 0.35f, t);
                line(box.left, y, chip.left, y);
                line(chip.right, y, box.right, y);
                const float x = Lerp(box.left + pad * 0.35f, box.right - pad * 0.35f, t);
                line(x, box.top, x, chip.top);
                line(x, chip.bottom, x, box.bottom);
            }
            break;
        }
        case Glyph::Memory: {
            const int rows = 3;
            const float gap = s * 0.06f;
            const float rowH = (h - gap * (rows - 1)) / rows;
            for (int i = 0; i < rows; ++i) {
                const float top = box.top + i * (rowH + gap);
                const D2D1_RECT_F r = D2D1::RectF(box.left + s * 0.12f, top, box.right - s * 0.12f,
                                                  top + rowH);
                StrokeRoundRect(ctx, r, rowH * 0.3f, color, strokeWidth);
                const float fillW = (r.right - r.left) * (i == 0 ? 0.85f : (i == 1 ? 0.55f : 0.7f));
                ctx->FillRectangle(D2D1::RectF(r.left, r.top, r.left + fillW, r.bottom), brush);
            }
            break;
        }
        case Glyph::NetworkIn:
        case Glyph::NetworkOut: {
            const float dir = glyph == Glyph::NetworkIn ? 1.0f : -1.0f;
            const float half = s * 0.30f;
            line(cx, cy - half * dir, cx, cy + half * dir);
            line(cx, cy + half * dir, cx - half * 0.6f, cy + half * 0.3f * dir);
            line(cx, cy + half * dir, cx + half * 0.6f, cy + half * 0.3f * dir);
            break;
        }
        case Glyph::ChevronUp: {
            line(cx - s * 0.28f, cy + s * 0.10f, cx, cy - s * 0.18f);
            line(cx + s * 0.28f, cy + s * 0.10f, cx, cy - s * 0.18f);
            break;
        }
        case Glyph::Up: {
            line(cx - s * 0.26f, cy + s * 0.12f, cx, cy - s * 0.16f);
            line(cx + s * 0.26f, cy + s * 0.12f, cx, cy - s * 0.16f);
            line(cx, cy - s * 0.16f, cx, cy + s * 0.30f);
            break;
        }
        case Glyph::Down: {
            line(cx - s * 0.26f, cy - s * 0.12f, cx, cy + s * 0.16f);
            line(cx + s * 0.26f, cy - s * 0.12f, cx, cy + s * 0.16f);
            line(cx, cy + s * 0.16f, cx, cy - s * 0.30f);
            break;
        }
        case Glyph::Gear: {
            ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.20f, s * 0.20f), brush,
                             strokeWidth);
            ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.36f, s * 0.36f), brush,
                             strokeWidth);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
                const float c = std::cos(a);
                const float sn = std::sin(a);
                line(cx + c * s * 0.20f, cy + sn * s * 0.20f, cx + c * s * 0.36f,
                     cy + sn * s * 0.36f);
            }
            break;
        }
        case Glyph::Close: {
            line(cx - s * 0.22f, cy - s * 0.22f, cx + s * 0.22f, cy + s * 0.22f);
            line(cx + s * 0.22f, cy - s * 0.22f, cx - s * 0.22f, cy + s * 0.22f);
            break;
        }
        case Glyph::Apps: {
            for (int i = 0; i < 4; ++i) {
                const float x = box.left + (i % 2) * s * 0.46f + s * 0.10f;
                const float y = box.top + (i / 2) * s * 0.46f + s * 0.10f;
                ctx->FillRectangle(
                    D2D1::RectF(x, y, x + s * 0.26f, y + s * 0.26f), brush);
            }
            break;
        }
        case Glyph::Bolt: {
            ID2D1PathGeometry* geo = nullptr;
            if (FAILED(factory->CreatePathGeometry(&geo))) break;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(geo->Open(&sink))) {
                geo->Release();
                break;
            }
            sink->BeginFigure(D2D1::Point2F(cx + s * 0.04f, box.top + s * 0.10f),
                              D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx - s * 0.26f, cy + s * 0.04f));
            sink->AddLine(D2D1::Point2F(cx - s * 0.02f, cy + s * 0.04f));
            sink->AddLine(D2D1::Point2F(cx - s * 0.10f, box.bottom - s * 0.10f));
            sink->AddLine(D2D1::Point2F(cx + s * 0.26f, cy - s * 0.06f));
            sink->AddLine(D2D1::Point2F(cx + s * 0.02f, cy - s * 0.06f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            ctx->FillGeometry(geo, brush);
            geo->Release();
            break;
        }
    }

    brush->Release();
}

}  // namespace dock
