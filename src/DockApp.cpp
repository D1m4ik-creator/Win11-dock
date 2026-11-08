#include "DockApp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dock {

namespace {

// All DIP at 96 dpi.
constexpr float kPillW = 62.0f;
constexpr float kPillH = 28.0f;
constexpr float kPillGap = 5.0f;
constexpr float kEdgePad = 4.0f;
constexpr float kCardW = 420.0f;
constexpr float kCardH = 332.0f;
constexpr float kCardGap = 8.0f;
constexpr float kMenuW = 228.0f;
constexpr float kMenuRowH = 30.0f;
constexpr float kPanelHeadroom = 380.0f;  // window space above the taskbar
constexpr float kSeparatorGap = 8.0f;
constexpr float kHoverOpenDelay = 350.0f;  // ms of dwell before the card pops

const wchar_t* const kClassName = L"Win11DockOverlay";
const wchar_t* const kFontFamily = L"Segoe UI Variable Text";

void FillText(ID2D1DeviceContext* ctx, IDWriteTextFormat* fmt, const std::wstring& text,
              const D2D1_RECT_F& box, ID2D1Brush* brush) {
    if (!ctx || !fmt || text.empty()) return;
    ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, box, brush,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

std::wstring MetricTitle(int metric) {
    switch (metric) {
        case kMetricCpu: return L"Процессор";
        case kMetricRam: return L"Память";
        case kMetricGpu: return L"Видеокарта";
        case kMetricNet: return L"Сеть";
        default: return L"?";
    }
}

}  // namespace

DockApp::~DockApp() { Shutdown(); }

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool DockApp::Init(HINSTANCE instance) {
    instance_ = instance;
    baseDir_ = ExeDir();
    config_ = Config::Load(baseDir_);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    theme_ = LoadTheme();
    if (config_.themeMode == ThemeMode::Light || config_.themeMode == ThemeMode::Dark) {
        theme_ = LoadTheme();
        theme_.light = config_.themeMode == ThemeMode::Light;
    }
    if (config_.accentOverride != 0) theme_.accent = ColorHex(config_.accentOverride);

    metrics_.Init();
    metrics_.Poll();
    lastPoll_ = GetTickCount64();

    if (!CreateDockWindow()) return false;

    if (!surface_.Create(hwnd_)) return false;
    surface_.SetDpi(dpi_);
    icons_.Init(surface_.D2D());

    CreateTextFormats();
    RefreshApps();
    Relayout();

    SetTimer(hwnd_, 1, 500, nullptr);
    timerMs_ = 500;
    return true;
}

void DockApp::Shutdown() {
    if (!hwnd_) {
        metrics_.Shutdown();
        return;
    }

    KillTimer(hwnd_, 1);
    ReleaseTextFormats();
    icons_.Clear();
    surface_.Destroy();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;

    metrics_.Shutdown();
    CoUninitialize();
}

int DockApp::Run() {
    MSG msg{};
    while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

bool DockApp::CreateDockWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DockApp::WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kClassName, L"Win11 Dock", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, instance_,
        this);
    return hwnd_ != nullptr;
}

void DockApp::CreateTextFormats() {
    IDWriteFactory* dw = surface_.DWrite();
    if (!dw) return;

    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                    IDWriteTextFormat** out) {
        IDWriteTextFormat* fmt = nullptr;
        if (FAILED(dw->CreateTextFormat(kFontFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", &fmt))) {
            if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", &fmt))) {
                return;
            }
        }
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        *out = fmt;
    };

    make(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, &fmtPill_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &fmtTitle_);
    make(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, &fmtBig_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &fmtBody_);
    make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &fmtSmall_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &fmtMenu_);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, &fmtLabelRight_);
    make(20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, &fmtValueRight_);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, &fmtSmallRight_);
}

void DockApp::ReleaseTextFormats() {
    auto rel = [](IDWriteTextFormat*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    rel(fmtPill_);
    rel(fmtTitle_);
    rel(fmtBig_);
    rel(fmtBody_);
    rel(fmtSmall_);
    rel(fmtMenu_);
    rel(fmtLabelRight_);
    rel(fmtValueRight_);
    rel(fmtSmallRight_);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void DockApp::Relayout() {
    shell_ = QueryShellLayout();
    if (!shell_.valid) {
        UpdateVisibility();
        return;
    }

    dpi_ = shell_.dpi > 0.0f ? shell_.dpi : 96.0f;
    const float scale = dpi_ / 96.0f;

    const RECT tb = shell_.taskbar;
    int left = shell_.freeLeft + static_cast<int>(2 * scale);
    int right = shell_.freeRight - static_cast<int>(4 * scale);

    // The band must not overlap the task list or the tray on either side.
    if (right - left < 40) {
        // Nothing sensible to do: hide rather than draw over shell chrome.
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        return;
    }

    bandH_ = static_cast<float>(tb.bottom - tb.top) / scale;
    bandW_ = static_cast<float>(right - left) / scale;

    const int top = tb.top - static_cast<int>(kPanelHeadroom * scale);
    const int bottom = tb.bottom;

    bandTop_ = static_cast<float>(tb.top - top) / scale;
    winW_ = bandW_;
    winH_ = static_cast<float>(bottom - top) / scale;

    if (windowPx_.left == left && windowPx_.top == top && windowPx_.right == right &&
        windowPx_.bottom == bottom && surface_.Width() == static_cast<UINT>(right - left)) {
        // Unchanged.
    } else {
        RECT r{left, top, right, bottom};
        windowPx_ = r;
        winPxW_ = right - left;
        winPxH_ = bottom - top;
        bandPxTop_ = tb.top - top;
        bandPxH_ = tb.bottom - tb.top;

        SetWindowPos(hwnd_, HWND_TOPMOST, left, top, winPxW_, winPxH_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        surface_.SetDpi(dpi_);
        surface_.Resize(static_cast<UINT>(winPxW_), static_cast<UINT>(winPxH_));
        dirty_ = true;
    }

    BuildItems();
    ApplyRegion();
    UpdateVisibility();
}

void DockApp::BuildItems() {
    // Rebuilding the list used to reset every hover weight to zero, and the
    // refresh timer rebuilds it every 700 ms: a pill the pointer rested on lit
    // up and dropped back in that rhythm, which is the blink the user saw. Keep
    // the current weights across a rebuild.
    float keptHover[kMetricCount] = {};
    for (const Item& old : items_) {
        if (old.kind == ItemKind::Metric && old.metric >= 0 && old.metric < kMetricCount) {
            keptHover[old.metric] = old.hover;
        }
    }

    items_.clear();

    const float scale = dpi_ / 96.0f;
    // Only the metric pills live in the strip. They are pinned to the right
    // edge of the free stretch: its width follows the running-app buttons and
    // changes as windows come and go (and as virtual desktops are switched), so
    // anything centred would slide around. Its right edge does not move.
    const float pillsW =
        static_cast<float>(kMetricCount) * kPillW + static_cast<float>(kMetricCount - 1) * kPillGap;
    float px = bandW_ - kEdgePad - pillsW;
    if (px < kEdgePad) px = kEdgePad;

    for (int m = 0; m < kMetricCount; ++m) {
        Item it;
        it.kind = ItemKind::Metric;
        it.metric = m;
        it.x = px;
        it.w = kPillW;
        it.hover = keptHover[m];
        items_.push_back(it);
        px += kPillW + kPillGap;
    }

    sepX_ = 1e6f;  // no app icons, so no separator
    (void)scale;
}

void DockApp::ApplyRegion() {
    if (!hwnd_) return;

    // The window region must cover exactly the interactive pixels. Clicks in
    // the empty stretch between the app icons and the metric pills have to
    // reach the taskbar underneath, and HTTRANSPARENT does not work across
    // processes, so that gap is cut out of the region entirely.
    //
    // Adjacent items are merged into one block: cutting between neighbouring
    // icons would generate a WM_MOUSELEAVE every time the pointer crosses the
    // seam and the hover animation would stutter.
    const float scale = dpi_ / 96.0f;
    HRGN region = CreateRectRgn(0, 0, 0, 0);

    const int top = bandPxTop_;
    const int bottom = bandPxTop_ + bandPxH_;
    int blockLeft = -1;
    int blockRight = -1;

    // Every block that goes into the region, kept so the previous result can be
    // compared against the new one.
    std::vector<RECT> blocks;

    auto flushBlock = [&]() {
        if (blockLeft < 0) return;
        HRGN block = CreateRectRgn(blockLeft, top, blockRight, bottom);
        if (block) {
            CombineRgn(region, region, block, RGN_OR);
            DeleteObject(block);
        }
        blocks.push_back(RECT{blockLeft, top, blockRight, bottom});
        blockLeft = -1;
        blockRight = -1;
    };

    for (const Item& it : items_) {
        const int l = static_cast<int>(it.x * scale);
        const int r = static_cast<int>((it.x + it.w) * scale);
        // Hover growth of an app icon and the pill border stay inside the cell.
        if (blockLeft >= 0 && l <= blockRight + 1) {
            if (r > blockRight) blockRight = r;
        } else {
            flushBlock();
            blockLeft = l;
            blockRight = r;
        }
    }
    flushBlock();

    if (panelOpen_) {
        const int pad = 6;
        int l = static_cast<int>(panelRect_.left * scale) - pad;
        int t = static_cast<int>(panelRect_.top * scale) - pad;
        int r = static_cast<int>(panelRect_.right * scale) + pad;
        int b = static_cast<int>(panelRect_.bottom * scale) + pad;
        HRGN panel = CreateRoundRectRgn(l, t, r + 1, b + 1, 22, 22);
        if (panel) {
            CombineRgn(region, region, panel, RGN_OR);
            DeleteObject(panel);
        }
        blocks.push_back(RECT{l, t, r, b});
    }

    // SetWindowRgn tears down and rebuilds the window's hit-test shape, and the
    // shell reports that as a spurious mouse-leave -- the pill under the pointer
    // then un-highlights and relights, which reads as flicker. Only touch the
    // region when the geometry actually changed.
    const bool same = blocks.size() == regionBlocks_.size() &&
                      std::equal(blocks.begin(), blocks.end(), regionBlocks_.begin(),
                                 [](const RECT& a, const RECT& b) {
                                     return a.left == b.left && a.top == b.top &&
                                            a.right == b.right && a.bottom == b.bottom;
                                 });
    if (same) {
        DeleteObject(region);
        return;
    }
    regionBlocks_ = blocks;

    SetWindowRgn(hwnd_, region, TRUE);  // window owns the region now
}

void DockApp::UpdateVisibility() {
    const bool fullscreen =
        config_.hideWhenFullscreen && IsForegroundFullscreen(hwnd_);

    const bool want = shell_.valid && shell_.taskbarVisible && !fullscreen;
    if (want == visible_) return;

    if (want) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    } else {
        ClosePanel();
        ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = want;
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Data refresh
// ---------------------------------------------------------------------------

void DockApp::RefreshApps() {
    windows_.Refresh();

    // Keep a stable order: existing apps stay put, new ones are appended, dead
    // ones are dropped.
    std::vector<AppEntry> ordered;
    ordered.reserve(windows_.Apps().size());

    std::vector<bool> used(windows_.Apps().size(), false);

    for (const std::wstring& key : appOrder_) {
        for (size_t i = 0; i < windows_.Apps().size(); ++i) {
            if (used[i]) continue;
            if (!lstrcmpiW(windows_.Apps()[i].exePath.c_str(), key.c_str())) {
                ordered.push_back(windows_.Apps()[i]);
                used[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < windows_.Apps().size(); ++i) {
        if (!used[i]) ordered.push_back(windows_.Apps()[i]);
    }

    appOrder_.clear();
    appOrder_.reserve(ordered.size());
    for (const AppEntry& e : ordered) appOrder_.push_back(e.exePath);

    apps_ = std::move(ordered);

    // No app icons are in the strip any more, so a changed window list does not
    // alter the layout: rebuilding the items would only reset the pill hover
    // weights and make the highlight blink.
    ApplyRegion();
}

void DockApp::RefreshTheme() {
    Theme t = LoadTheme();
    if (config_.themeMode == ThemeMode::Light) t.light = true;
    if (config_.themeMode == ThemeMode::Dark) t.light = false;
    if (config_.accentOverride != 0) t.accent = ColorHex(config_.accentOverride);
    theme_ = t;
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void DockApp::OnTick() {
    now_ = GetTickCount64();

    bool animating = false;

    // The shell periodically re-inserts Shell_TrayWnd into the topmost band,
    // which pushes this window below the taskbar: clicks then reach the
    // taskbar instead of the icons. Cheaper to re-assert cheaply every second
    // than to hunt HWND_TOPMOST messages. While a card is up the same thing has
    // to happen more often, or the card ends up drawn underneath whatever the
    // user had focused when it opened.
    const ULONGLONG topmostEvery = panelOpen_ ? 250 : 900;
    if (hwnd_ && visible_ && now_ - lastTopmost_ > topmostEvery) {
        lastTopmost_ = now_;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // The hover highlight and the card lifetime are both driven by the real
    // pointer position rather than by WM_MOUSELEAVE: the shell emits spurious
    // leaves whenever the window region is rebuilt, and trusting them made the
    // pill blink while the pointer sat still on it.
    if (now_ - lastCursor_ > 60) {
        lastCursor_ = now_;
        POINT cp{};
        if (GetCursorPos(&cp)) {
            POINT cl = cp;
            ScreenToClient(hwnd_, &cl);
            const float scale = dpi_ / 96.0f;
            const float fx = static_cast<float>(cl.x) / scale;
            const float fy = static_cast<float>(cl.y) / scale;

            const int hit = HitItem(fx, fy);
            if (hit != hoverIndex_) {
                hoverIndex_ = hit;
                dirty_ = true;
            }
            if (hit != hoverDwellItem_) {
                hoverDwellItem_ = hit;
                hoverSince_ = now_;
            }

            if (panelOpen_) {
                const bool insideCard = HitPanel(fx, fy);
                if (!insideCard && hit != panelItem_) ClosePanel();
            } else if (hit >= 0 && hit < static_cast<int>(items_.size()) &&
                       items_[hit].kind == ItemKind::Metric &&
                       now_ - hoverSince_ >= static_cast<ULONGLONG>(kHoverOpenDelay)) {
                OpenCard(hit);
            }
        }
    }

    // Metric sampling: cheap enough to call every tick, throttled internally.
    const Sample before = metrics_.Current();
    metrics_.Poll();
    const Sample& after = metrics_.Current();
    if (after.cpuPercent != before.cpuPercent || after.ramPercent != before.ramPercent ||
        after.gpuPercent != before.gpuPercent ||
        after.netInBytesPerSec != before.netInBytesPerSec ||
        after.netOutBytesPerSec != before.netOutBytesPerSec ||
        after.cpuGHz != before.cpuGHz) {
        dirty_ = true;
    }

    if (now_ - lastApps_ > 700) {
        lastApps_ = now_;
        const size_t beforeCount = apps_.size();
        RefreshApps();
        if (apps_.size() != beforeCount) dirty_ = true;
        for (size_t i = 0; i < apps_.size() && i < items_.size(); ++i) {
            if (items_[i].kind == ItemKind::App && items_[i].foreground != apps_[i].foreground) {
                dirty_ = true;
            }
        }
    }

    if (now_ - lastLayout_ > 1000) {
        lastLayout_ = now_;
        ShellLayout check = QueryShellLayout();
        if (!check.valid || check.freeLeft != shell_.freeLeft ||
            check.freeRight != shell_.freeRight || check.taskbar.top != shell_.taskbar.top ||
            check.taskbar.bottom != shell_.taskbar.bottom ||
            check.taskbarVisible != shell_.taskbarVisible) {
            Relayout();
        } else {
            UpdateVisibility();
        }
    }

    if (now_ - lastTheme_ > 2000) {
        lastTheme_ = now_;
        const bool light = ReadRegDword(HKEY_CURRENT_USER,
                                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes"
                                        L"\\Personalize",
                                        L"SystemUsesLightTheme", 0) != 0;
        if (light != theme_.light) RefreshTheme();
    }

    // Hover easing.
    for (size_t i = 0; i < items_.size(); ++i) {
        const float target = (static_cast<int>(i) == hoverIndex_) ? 1.0f : 0.0f;
        if (std::fabs(items_[i].hover - target) > 0.002f) {
            items_[i].hover = Approach(items_[i].hover, target, 0.30f, 0.016f);
            animating = true;
            dirty_ = true;
        } else {
            items_[i].hover = target;
        }
    }

    // Panel easing.
    const float panelTarget = panelOpen_ ? 1.0f : 0.0f;
    if (std::fabs(panelT_ - panelTarget) > 0.002f) {
        panelT_ = Approach(panelT_, panelTarget, 0.35f, 0.016f);
        animating = true;
        dirty_ = true;
    } else {
        panelT_ = panelTarget;
    }

    // The pointer poll that drives the highlight lives in this timer, so the
    // idle tick cannot be as lazy as it used to be -- at 500 ms the pill would
    // light up half a second after the pointer arrived. 100 ms is fast enough
    // to feel immediate and still cheap: one GetCursorPos plus a few rect tests.
    const UINT want = (animating || panelOpen_) ? 16 : 100;
    if (timerMs_ != want) SetTimerInterval(want);

    if (dirty_ || animating) {
        dirty_ = false;
        Paint();
    }
}

void DockApp::SetTimerInterval(UINT ms) {
    timerMs_ = ms;
    SetTimer(hwnd_, 1, ms, nullptr);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void DockApp::Paint() {
    if (!surface_.Ready()) return;
    if (!surface_.BeginFrame()) return;

    ID2D1DeviceContext* ctx = surface_.D2D();

    ID2D1SolidColorBrush* brush = nullptr;
    ctx->CreateSolidColorBrush(theme_.text, &brush);
    if (!brush) {
        surface_.EndFrame();
        return;
    }

    PaintItems(ctx, brush);

    if (panelT_ > 0.002f) {
        if (panelMode_ == PanelMode::Card) {
            PaintCard(ctx, brush);
        } else if (panelMode_ == PanelMode::Menu) {
            PaintMenu(ctx, brush);
        }
    }

    brush->Release();
    surface_.EndFrame();
}

void DockApp::PaintItems(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush) {
    for (const Item& item : items_) {
        PaintMetricPill(ctx, brush, item);
    }
}

void DockApp::PaintMetricPill(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush,
                              const Item& item) {
    const float cy = bandTop_ + bandH_ * 0.5f;
    const float t = item.hover;
    const Sample& s = metrics_.Current();

    D2D1_RECT_F r = D2D1::RectF(item.x, cy - kPillH * 0.5f, item.x + item.w,
                                cy + kPillH * 0.5f);

    // Resting pill is very subtle so it reads as part of the taskbar; hover
    // lights it up the way the user described.
    const float baseAlpha = theme_.light ? 0.05f : 0.07f;
    brush->SetColor(WithAlpha(theme_.light ? ColorHex(0x000000) : ColorHex(0xFFFFFF),
                              Lerp(baseAlpha, 0.16f, t)));
    FillRoundRect(ctx, r, 7.0f, brush->GetColor());

    if (t > 0.02f) {
        brush->SetColor(WithAlpha(theme_.accent, 0.55f * t));
        StrokeRoundRect(ctx, r, 7.0f, brush->GetColor(), 1.0f);
    }

    std::wstring value;
    switch (item.metric) {
        case kMetricCpu: value = FormatPercent(s.cpuPercent); break;
        case kMetricRam: value = FormatPercent(s.ramPercent); break;
        case kMetricGpu: value = s.gpuValid ? FormatPercent(s.gpuPercent) : L"--"; break;
        case kMetricNet: {
            // KB/s is a lot wider than the other readings, and the pill only has
            // room for a few characters: drop the unit and keep the number.
            const double kb = (s.netInBytesPerSec + s.netOutBytesPerSec) / 1024.0;
            wchar_t buf[32]{};
            swprintf(buf, ARRAYSIZE(buf), kb >= 1000.0 ? L"%.0f" : (kb >= 10.0 ? L"%.0f" : L"%.1f"),
                     kb);
            value = buf;
            break;
        }
        default: break;
    }

    const float iconBox = 13.0f;
    const float iconX = r.left + 6.0f;
    Glyph g = Glyph::Cpu;
    if (item.metric == kMetricRam) g = Glyph::Memory;
    else if (item.metric == kMetricGpu) g = Glyph::Bolt;
    else if (item.metric == kMetricNet) g = Glyph::NetworkIn;

    brush->SetColor(WithAlpha(t > 0.4f ? theme_.accent : theme_.textMuted,
                              Lerp(0.75f, 1.0f, t)));
    DrawGlyph(ctx, g,
              D2D1::RectF(iconX, cy - iconBox * 0.5f, iconX + iconBox, cy + iconBox * 0.5f),
              brush->GetColor(), 1.2f);

    if (fmtPill_) {
        brush->SetColor(theme_.text);
        D2D1_RECT_F textBox = D2D1::RectF(iconX + iconBox + 3.0f, r.top,
                                          r.right - 6.0f, r.bottom);
        FillText(ctx, fmtPill_, value, textBox, brush);
    }
}

void DockApp::PaintCard(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush) {
    const float scale = dpi_ / 96.0f;
    (void)scale;

    const float t = panelT_;
    const float slide = (1.0f - t) * 6.0f;

    D2D1_RECT_F r = panelRect_;
    r.top += slide;
    r.bottom += slide;

    const float alpha = config_.cardOpacity * t;

    // Drop shadow.
    for (int i = 3; i >= 1; --i) {
        D2D1_RECT_F s = r;
        s.left -= i * 1.5f;
        s.top -= i * 1.5f;
        s.right += i * 1.5f;
        s.bottom += i * 1.5f;
        brush->SetColor(WithAlpha(theme_.shadow, 0.10f * t));
        FillRoundRect(ctx, s, 10.0f, brush->GetColor());
    }

    brush->SetColor(WithAlpha(theme_.cardBg, alpha));
    FillRoundRect(ctx, r, 8.0f, brush->GetColor());
    brush->SetColor(WithAlpha(theme_.cardBorder, alpha));
    StrokeRoundRect(ctx, r, 8.0f, brush->GetColor(), 1.0f);

    const int metric = panelItem_ >= 0 && panelItem_ < static_cast<int>(items_.size()) &&
                               items_[panelItem_].kind == ItemKind::Metric
                           ? items_[panelItem_].metric
                           : kMetricCpu;

    const Sample& s = metrics_.Current();
    const float pad = 20.0f;
    const float footerH = 38.0f;
    const float contentBottom = r.bottom - footerH;
    const float innerW = r.right - r.left - pad * 2.0f;

    // Vertical rhythm, top to bottom: icon row, graph, two read-out rows.
    const float iconTop = r.top + pad;
    const float graphTop = iconTop + 30.0f;
    const float graphH = 150.0f;
    const float graphBottom = graphTop + graphH;
    const float readoutTop = graphBottom + 20.0f;

    // Header icon, echoing the PowerToys layout.
    {
        Glyph g = Glyph::Cpu;
        if (metric == kMetricRam) g = Glyph::Memory;
        else if (metric == kMetricGpu) g = Glyph::Bolt;
        else if (metric == kMetricNet) g = Glyph::NetworkIn;
        const float box = 22.0f;
        DrawGlyph(ctx, g, D2D1::RectF(r.left + pad, iconTop, r.left + pad + box, iconTop + box),
                  WithAlpha(theme_.textMuted, alpha), 1.6f);
    }

    // The graph is the hero element, spanning the full inner width.
    const D2D1_RECT_F graph = D2D1::RectF(r.left + pad, graphTop, r.right - pad, graphBottom);
    Series series = Series::Cpu;
    switch (metric) {
        case kMetricCpu: series = Series::Cpu; break;
        case kMetricRam: series = Series::Ram; break;
        case kMetricGpu: series = Series::Gpu; break;
        case kMetricNet: series = Series::NetIn; break;
        default: break;
    }
    PaintGraph(ctx, graph, series, metric);

    // Two read-out pairs under the graph: primary on the left, secondary right.
    std::wstring leftLabel;
    std::wstring leftValue;
    std::wstring rightLabel;
    std::wstring rightValue;
    std::wstring leftSub;
    std::wstring rightSub;

    switch (metric) {
        case kMetricCpu:
            leftLabel = L"Использование";
            leftValue = FormatPercent(s.cpuPercent);
            rightLabel = L"Скорость";
            rightValue = s.cpuGHz > 0.0f ? FormatGhz(s.cpuGHz) + L" ГГц" : L"--";
            break;
        case kMetricRam:
            leftLabel = L"Используемая";
            leftValue = FormatGb(s.ramUsedGB);
            leftSub = FormatGb(std::max(0.0f, s.ramTotalGB - s.ramUsedGB)) + L" свободно";
            rightLabel = L"Всего";
            rightValue = FormatGb(s.ramTotalGB);
            rightSub = L"Загрузка " + FormatPercent(s.ramPercent);
            break;
        case kMetricGpu:
            leftLabel = L"Использование";
            leftValue = s.gpuValid ? FormatPercent(s.gpuPercent) : L"--";
            leftSub = L"Intel(R) UHD Graphics";
            rightLabel = L"Движок";
            rightValue = L"3D";
            break;
        case kMetricNet:
            leftLabel = L"Приём";
            leftValue = FormatKbPerSec(s.netInBytesPerSec);
            rightLabel = L"Отдача";
            rightValue = FormatKbPerSec(s.netOutBytesPerSec);
            break;
        default: break;
    }

    const float colW = innerW * 0.5f - 8.0f;

    brush->SetColor(WithAlpha(theme_.textMuted, alpha));
    FillText(ctx, fmtSmall_, leftLabel,
             D2D1::RectF(r.left + pad, readoutTop, r.left + pad + colW, readoutTop + 16.0f),
             brush);
    FillText(ctx, fmtSmallRight_, rightLabel,
             D2D1::RectF(r.right - pad - colW, readoutTop, r.right - pad, readoutTop + 16.0f),
             brush);

    brush->SetColor(WithAlpha(theme_.text, alpha));
    FillText(ctx, fmtBig_, leftValue,
             D2D1::RectF(r.left + pad, readoutTop + 15.0f, r.left + pad + colW,
                         readoutTop + 45.0f),
             brush);
    FillText(ctx, fmtValueRight_, rightValue,
             D2D1::RectF(r.right - pad - colW, readoutTop + 15.0f, r.right - pad,
                         readoutTop + 45.0f),
             brush);

    if (!leftSub.empty() || !rightSub.empty()) {
        brush->SetColor(WithAlpha(theme_.textMuted, alpha));
        if (!leftSub.empty()) {
            FillText(ctx, fmtSmall_,
                     leftSub, D2D1::RectF(r.left + pad, readoutTop + 45.0f,
                                          r.left + pad + colW, readoutTop + 63.0f),
                     brush);
        }
        if (!rightSub.empty()) {
            FillText(ctx, fmtSmallRight_,
                     rightSub, D2D1::RectF(r.right - pad - colW, readoutTop + 45.0f,
                                           r.right - pad, readoutTop + 63.0f),
                     brush);
        }
    }

    // Footer: title on the left, the shortcut hint on the right.
    {
        ID2D1SolidColorBrush* sep = nullptr;
        ctx->CreateSolidColorBrush(WithAlpha(theme_.Separator(), alpha), &sep);
        if (sep) {
            ctx->DrawLine(D2D1::Point2F(r.left, contentBottom), D2D1::Point2F(r.right, contentBottom),
                          sep, 1.0f);
            sep->Release();
        }
    }

    brush->SetColor(WithAlpha(theme_.text, alpha));
    FillText(ctx, fmtBody_, MetricTitle(metric),
             D2D1::RectF(r.left + pad, contentBottom + 9.0f, r.right - pad - 170.0f,
                         r.bottom - 7.0f),
             brush);

    brush->SetColor(WithAlpha(theme_.textMuted, alpha));
    FillText(ctx, fmtSmallRight_, L"Открыть диспетчер задач  \u21B5",
             D2D1::RectF(r.right - pad - 180.0f, contentBottom + 9.0f, r.right - pad,
                         r.bottom - 7.0f),
             brush);
}

void DockApp::PaintGraph(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, Series series,
                         int metric) {
    const float alpha = panelT_ * config_.cardOpacity;

    // Baseline.
    {
        ID2D1SolidColorBrush* b = nullptr;
        ctx->CreateSolidColorBrush(WithAlpha(theme_.Separator(), alpha), &b);
        if (b) {
            ctx->DrawLine(D2D1::Point2F(r.left, r.bottom), D2D1::Point2F(r.right, r.bottom), b,
                          1.0f);
            b->Release();
        }
    }

    // Auto-range so a quiet machine still shows a readable curve: scale to the
    // window's own peak, with a floor so idle noise does not fill the frame.
    auto rangeOf = [](const std::deque<float>& data) {
        float peak = 0.0f;
        for (float v : data) peak = std::max(peak, v);
        const float padded = peak * 1.25f;
        return std::min(100.0f, std::max(10.0f, padded));
    };

    auto draw = [&](const std::deque<float>& data, const D2D1_COLOR_F& color, bool fill) {
        if (data.size() < 2) return;
        const float w = r.right - r.left;
        const float h = r.bottom - r.top;
        const float step = w / static_cast<float>(data.size() - 1);
        const float maxV = rangeOf(data);

        ID2D1SolidColorBrush* b = nullptr;
        ctx->CreateSolidColorBrush(WithAlpha(color, alpha), &b);
        if (!b) return;

        if (fill) {
            // The area under the curve is a soft wash so the line stays the
            // readable element, the way PowerToys draws its charts.
            ID2D1SolidColorBrush* wash = nullptr;
            ctx->CreateSolidColorBrush(WithAlpha(color, alpha * 0.18f), &wash);
            ID2D1Factory* factory = nullptr;
            ctx->GetFactory(&factory);
            ID2D1PathGeometry* geo = nullptr;
            if (wash && factory && SUCCEEDED(factory->CreatePathGeometry(&geo))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(geo->Open(&sink))) {
                    sink->BeginFigure(
                        D2D1::Point2F(r.left, r.bottom),
                        D2D1_FIGURE_BEGIN_FILLED);
                    auto cur = data.begin();
                    for (size_t i = 0; i < data.size(); ++i, ++cur) {
                        const float v = ClampF(*cur / maxV, 0.0f, 1.0f);
                        sink->AddLine(
                            D2D1::Point2F(r.left + step * static_cast<float>(i),
                                          r.bottom - v * h));
                    }
                    sink->AddLine(D2D1::Point2F(r.right, r.bottom));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();
                    ctx->FillGeometry(geo, wash);
                }
                geo->Release();
            }
            if (wash) wash->Release();
        }

        auto cur = data.begin();
        for (size_t i = 0; i + 1 < data.size(); ++i, ++cur) {
            const float v0 = ClampF(*cur / maxV, 0.0f, 1.0f);
            const float v1 = ClampF(*(cur + 1) / maxV, 0.0f, 1.0f);
            ctx->DrawLine(D2D1::Point2F(r.left + step * static_cast<float>(i), r.bottom - v0 * h),
                          D2D1::Point2F(r.left + step * static_cast<float>(i + 1),
                                        r.bottom - v1 * h),
                          b, 1.8f);
        }
        b->Release();
    };

    if (metric == kMetricNet) {
        draw(metrics_.History(Series::NetOut), theme_.textMuted, false);
        draw(metrics_.History(Series::NetIn), theme_.accent, false);
    } else {
        draw(metrics_.History(series), theme_.accent, true);
    }
}

void DockApp::PaintMenu(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush) {
    const float t = panelT_;
    const float alpha = config_.cardOpacity * t;
    const float slide = (1.0f - t) * 6.0f;

    D2D1_RECT_F r = panelRect_;
    r.top += slide;
    r.bottom += slide;

    for (int i = 3; i >= 1; --i) {
        D2D1_RECT_F s = r;
        s.left -= i * 1.5f;
        s.top -= i * 1.5f;
        s.right += i * 1.5f;
        s.bottom += i * 1.5f;
        brush->SetColor(WithAlpha(theme_.shadow, 0.10f * t));
        FillRoundRect(ctx, s, 10.0f, brush->GetColor());
    }

    brush->SetColor(WithAlpha(theme_.cardBg, alpha));
    FillRoundRect(ctx, r, 8.0f, brush->GetColor());
    brush->SetColor(WithAlpha(theme_.cardBorder, alpha));
    StrokeRoundRect(ctx, r, 8.0f, brush->GetColor(), 1.0f);

    const float pad = 8.0f;
    float y = r.top + 6.0f;
    for (size_t i = 0; i < menuRows_.size(); ++i) {
        D2D1_RECT_F row = D2D1::RectF(r.left + pad * 0.5f, y, r.right - pad * 0.5f,
                                      y + kMenuRowH);
        if (static_cast<int>(i) == menuHover_) {
            brush->SetColor(WithAlpha(theme_.hoverBg, alpha * 1.4f));
            FillRoundRect(ctx, row, 5.0f, brush->GetColor());
        }
        brush->SetColor(WithAlpha(theme_.text, alpha));
        FillText(ctx, fmtMenu_, menuRows_[i],
                 D2D1::RectF(row.left + 10.0f, row.top, row.right - 8.0f, row.bottom), brush);
        y += kMenuRowH;
    }
}

// ---------------------------------------------------------------------------
// Hit testing / interaction
// ---------------------------------------------------------------------------

POINT DockApp::ToDip(LPARAM lp) const {
    const float scale = dpi_ / 96.0f;
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    p.x = static_cast<LONG>(static_cast<float>(p.x) / scale);
    p.y = static_cast<LONG>(static_cast<float>(p.y) / scale);
    return p;
}

int DockApp::HitItem(float x, float y) const {
    if (y < bandTop_ || y > bandTop_ + bandH_) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (x >= it.x && x <= it.x + it.w) return static_cast<int>(i);
    }
    return -1;
}

int DockApp::HitMenuRow(float x, float y) const {
    if (!panelOpen_ || panelMode_ != PanelMode::Menu) return -1;
    if (x < panelRect_.left || x > panelRect_.right || y < panelRect_.top ||
        y > panelRect_.bottom) {
        return -1;
    }
    const int idx = static_cast<int>((y - panelRect_.top - 6.0f) / kMenuRowH);
    if (idx < 0 || idx >= static_cast<int>(menuRows_.size())) return -1;
    return idx;
}

bool DockApp::HitPanel(float x, float y) const {
    if (!panelOpen_) return false;
    return x >= panelRect_.left && x <= panelRect_.right && y >= panelRect_.top &&
           y <= panelRect_.bottom;
}

const AppEntry* DockApp::AppForPid(DWORD pid) const {
    for (const AppEntry& e : apps_) {
        if (e.pid == pid) return &e;
    }
    return nullptr;
}

void DockApp::OpenCard(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items_.size())) return;

    if (panelOpen_ && panelMode_ == PanelMode::Card && panelItem_ == itemIndex) {
        ClosePanel();
        return;
    }

    const Item& it = items_[itemIndex];

    // The card is wider than the strip, so it is allowed to overhang the band
    // horizontally as long as it stays inside the overlay window.
    const float w = std::min(kCardW, winW_ - 2.0f);
    const float cxBand = it.x + it.w * 0.5f;
    float x = cxBand - w * 0.5f;
    x = ClampF(x, 1.0f, std::max(1.0f, winW_ - w - 1.0f));

    panelRect_ = D2D1::RectF(x, bandTop_ - kCardGap - kCardH, x + w,
                             bandTop_ - kCardGap);
    panelMode_ = PanelMode::Card;
    panelItem_ = itemIndex;
    panelOpen_ = true;
    menuRows_.clear();
    ApplyRegion();
    dirty_ = true;
}

void DockApp::BuildMenu(int itemIndex) {
    menuRows_.clear();

    if (itemIndex >= 0 && itemIndex < static_cast<int>(items_.size())) {
        const Item& it = items_[itemIndex];
        if (it.kind == ItemKind::App) {
            menuRows_.push_back(L"Открыть");
            menuRows_.push_back(L"Свернуть");
            menuRows_.push_back(L"Закрыть окно");
            menuRows_.push_back(L"Диспетчер задач");
        } else {
            switch (it.metric) {
                case kMetricCpu:
                    menuRows_.push_back(L"Диспетчер задач");
                    break;
                case kMetricRam:
                    menuRows_.push_back(L"Диспетчер задач");
                    menuRows_.push_back(L"Параметры памяти");
                    break;
                case kMetricGpu:
                    menuRows_.push_back(L"Диспетчер задач");
                    break;
                case kMetricNet:
                    menuRows_.push_back(L"Сетевые подключения");
                    break;
                default: break;
            }
            menuRows_.push_back(L"Обновить показатели");
        }
    }

    menuRows_.push_back(L"Выход");
}

void DockApp::OpenMenu(int itemIndex, float x, float y) {
    BuildMenu(itemIndex);

    const float h = static_cast<float>(menuRows_.size()) * kMenuRowH + 12.0f;
    float w = std::min(kMenuW, bandW_ - 2.0f);
    float left = ClampF(x - w * 0.5f, 1.0f, std::max(1.0f, bandW_ - w - 1.0f));

    panelRect_ = D2D1::RectF(left, bandTop_ - kCardGap - h, left + w, bandTop_ - kCardGap);
    panelMode_ = PanelMode::Menu;
    panelItem_ = itemIndex;
    panelOpen_ = true;
    menuHover_ = -1;
    ApplyRegion();
    dirty_ = true;
}

void DockApp::ClosePanel() {
    if (!panelOpen_) return;
    panelOpen_ = false;
    menuRows_.clear();
    menuHover_ = -1;
    ApplyRegion();
    dirty_ = true;
}

void DockApp::RunMenuRow(int row) {
    if (row < 0 || row >= static_cast<int>(menuRows_.size())) return;

    const std::wstring text = menuRows_[row];
    const int itemIndex = panelItem_;
    const bool isApp = itemIndex >= 0 && itemIndex < static_cast<int>(items_.size()) &&
                       items_[itemIndex].kind == ItemKind::App;

    ClosePanel();

    if (text == L"Выход") {
        running_ = false;
        PostQuitMessage(0);
        return;
    }
    if (text == L"Диспетчер задач") {
        Metrics::OpenTaskManager();
        return;
    }
    if (text == L"Обновить показатели") {
        metrics_.Poll();
        dirty_ = true;
        return;
    }
    if (text == L"Параметры памяти") {
        ShellExecuteW(nullptr, L"open", L"ms-settings:about", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (text == L"Сетевые подключения") {
        ShellExecuteW(nullptr, L"open", L"ms-settings:network-status", nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
    }
    if (!isApp) return;

    const Item& it = items_[itemIndex];
    const AppEntry* app = AppForPid(it.pid);
    if (!app) return;

    if (text == L"Открыть") {
        AppEntry copy = *app;
        WindowList::Activate(copy);
    } else if (text == L"Свернуть") {
        HWND w = app->CurrentWindow();
        if (w) ShowWindow(w, SW_MINIMIZE);
    } else if (text == L"Закрыть окно") {
        HWND w = app->CurrentWindow();
        if (w) WindowList::CloseWindow(w);
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK DockApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    auto* self = reinterpret_cast<DockApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMessage(hwnd, msg, wp, lp);
}

LRESULT DockApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_NCHITTEST: {
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT client = screen;
            ScreenToClient(hwnd_, &client);
            const float scale = dpi_ / 96.0f;
            const float x = static_cast<float>(client.x) / scale;
            const float y = static_cast<float>(client.y) / scale;

            if (HitPanel(x, y)) return HTCLIENT;
            if (HitItem(x, y) >= 0) return HTCLIENT;
            // The band strip itself: let the taskbar have the clicks.
            return HTTRANSPARENT;
        }

        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            // Highlighting and card lifetime run off the pointer poll in OnTick,
            // so no state changes here: two writers to hoverIndex_ made the pill
            // flicker. Only the menu row is cheap enough to track directly.
            const POINT p = ToDip(lp);
            const float fx = static_cast<float>(p.x);
            const float fy = static_cast<float>(p.y);

            if (panelOpen_ && panelMode_ == PanelMode::Menu) {
                const int row = HitMenuRow(fx, fy);
                if (row != menuHover_) {
                    menuHover_ = row;
                    dirty_ = true;
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            tracking_ = false;
            // Nothing to reset: the highlight follows the pointer poll in OnTick.
            // Reacting to this message made the pill blink, because rebuilding
            // the window region produces a leave even when the pointer stays.
            if (panelMode_ == PanelMode::Menu && menuHover_ != -1) {
                menuHover_ = -1;
                dirty_ = true;
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT p = ToDip(lp);
            pressIndex_ = HitItem(static_cast<float>(p.x), static_cast<float>(p.y));
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT p = ToDip(lp);
            const float x = static_cast<float>(p.x);
            const float y = static_cast<float>(p.y);

            if (panelOpen_ && panelMode_ == PanelMode::Menu) {
                const int row = HitMenuRow(x, y);
                if (row >= 0) {
                    RunMenuRow(row);
                } else {
                    ClosePanel();
                }
                pressIndex_ = -1;
                return 0;
            }

            if (panelOpen_ && panelMode_ == PanelMode::Card && HitPanel(x, y)) {
                pressIndex_ = -1;
                return 0;
            }

            const int hit = HitItem(x, y);
            if (hit >= 0 && hit == pressIndex_) {
                const Item& it = items_[hit];
                if (it.kind == ItemKind::App) {
                    if (const AppEntry* app = AppForPid(it.pid)) {
                        AppEntry copy = *app;
                        WindowList::Activate(copy);
                    }
                } else {
                    OpenCard(hit);
                }
            } else if (panelOpen_) {
                ClosePanel();
            }

            pressIndex_ = -1;
            dirty_ = true;
            return 0;
        }

        case WM_RBUTTONUP: {
            const POINT p = ToDip(lp);
            const float x = static_cast<float>(p.x);
            const float y = static_cast<float>(p.y);

            if (panelOpen_) {
                ClosePanel();
                return 0;
            }
            const int hit = HitItem(x, y);
            if (hit >= 0) OpenMenu(hit, x, y);
            return 0;
        }

        case WM_TIMER:
            if (wp == 1) OnTick();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            Relayout();
            return 0;

        case WM_SETTINGCHANGE: {
            RefreshTheme();
            Relayout();
            return 0;
        }

        case WM_WINDOWPOSCHANGING: {
            // Stay pinned to the top of the z-order without fighting the shell.
            auto* wp2 = reinterpret_cast<WINDOWPOS*>(lp);
            wp2->flags |= SWP_NOACTIVATE;
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                ClosePanel();
                return 0;
            }
            break;

        case WM_DESTROY:
            running_ = false;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace dock
