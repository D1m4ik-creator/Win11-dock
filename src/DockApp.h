#pragma once

#include "Common.h"
#include "Metrics.h"
#include "Surface.h"
#include "WindowList.h"

namespace dock {

enum class ItemKind { App, Metric };

enum MetricId {
    kMetricCpu = 0,
    kMetricRam,
    kMetricGpu,
    kMetricNet,
    kMetricCount
};

// Overlay for the empty stretch of the taskbar between the running-app buttons
// (MSTaskListWClass) and the tray cluster (TrayNotifyWnd).
//
// One WS_EX_NOREDIRECTIONBITMAP window drawn through a DirectComposition
// surface: empty pixels really are empty, and WM_NCHITTEST returns HTTRANSPARENT
// outside the interactive bits so clicks fall through to the taskbar.
//
// The window is taller than the taskbar because the metric card pops up above
// the strip; that region stays transparent while no card is open.
class DockApp {
public:
    DockApp() = default;
    ~DockApp();

    bool Init(HINSTANCE instance);
    void Shutdown();
    int Run();

private:
    enum class PanelMode { Card, Menu };

    struct Item {
        ItemKind kind = ItemKind::App;
        int metric = kMetricCpu;  // valid for ItemKind::Metric
        DWORD pid = 0;            // valid for ItemKind::App
        float x = 0.0f;           // DIP, left edge inside the band
        float w = 0.0f;
        float hover = 0.0f;       // 0..1 animated
        bool foreground = false;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool CreateDockWindow();
    void CreateTextFormats();
    void ReleaseTextFormats();

    void Relayout();
    void ApplyRegion();
    void BuildItems();
    void RefreshApps();
    void RefreshTheme();
    void UpdateVisibility();

    void OnTick();
    void Paint();
    void PaintItems(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush);
    void PaintMetricPill(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush, const Item& item);
    void PaintCard(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush);
    void PaintMenu(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush);
    void PaintGraph(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, Series series, int metric);

    int HitItem(float x, float y) const;
    int HitMenuRow(float x, float y) const;
    bool HitPanel(float x, float y) const;

    POINT ToDip(LPARAM lp) const;

    const AppEntry* AppForPid(DWORD pid) const;

    void OpenCard(int itemIndex);
    void OpenMenu(int itemIndex, float x, float y);
    void ClosePanel();
    void BuildMenu(int itemIndex);
    void RunMenuRow(int row);

    void SetTimerInterval(UINT ms);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    Surface surface_;

    Metrics metrics_;
    WindowList windows_;
    IconCache icons_;
    Theme theme_{};
    Config config_{};
    std::wstring baseDir_;

    ShellLayout shell_{};
    float dpi_ = 96.0f;
    float bandH_ = 0.0f;    // taskbar height, DIP
    float bandW_ = 0.0f;    // free stretch width, DIP
    float bandTop_ = 0.0f;  // y of the taskbar strip inside the window, DIP
    float winW_ = 0.0f;
    float winH_ = 0.0f;
    RECT windowPx_{};
    int winPxW_ = 0;
    int winPxH_ = 0;
    int bandPxTop_ = 0;
    int bandPxH_ = 0;

    std::vector<AppEntry> apps_;
    std::vector<Item> items_;
    std::vector<std::wstring> appOrder_;  // stable icon order across refreshes
    int hoverIndex_ = -1;
    int pressIndex_ = -1;
    bool tracking_ = false;
    float sepX_ = 1e6f;  // separator between app icons and metric pills

    PanelMode panelMode_ = PanelMode::Card;
    bool panelOpen_ = false;
    int panelItem_ = -1;
    float panelT_ = 0.0f;
    D2D1_RECT_F panelRect_{};
    ULONGLONG hoverSince_ = 0;    // when the cursor entered the current pill
    int hoverDwellItem_ = -1;     // pill the dwell timer belongs to
    std::vector<std::wstring> menuRows_;
    int menuHover_ = -1;

    IDWriteTextFormat* fmtPill_ = nullptr;
    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtBig_ = nullptr;
    IDWriteTextFormat* fmtBody_ = nullptr;
    IDWriteTextFormat* fmtSmall_ = nullptr;
    IDWriteTextFormat* fmtMenu_ = nullptr;
    IDWriteTextFormat* fmtLabelRight_ = nullptr;
    IDWriteTextFormat* fmtValueRight_ = nullptr;
    IDWriteTextFormat* fmtSmallRight_ = nullptr;

    bool running_ = true;
    bool visible_ = false;
    bool dirty_ = true;
    UINT timerMs_ = 0;
    ULONGLONG now_ = 0;
    ULONGLONG lastPoll_ = 0;
    ULONGLONG lastApps_ = 0;
    ULONGLONG lastLayout_ = 0;
    ULONGLONG lastTheme_ = 0;
    ULONGLONG lastTopmost_ = 0;
    ULONGLONG lastCursor_ = 0;

    // Blocks that currently make up the window region, in device pixels. Used to
    // skip SetWindowRgn when nothing changed: every call produces a spurious
    // mouse-leave that used to make the hovered pill blink.
    std::vector<RECT> regionBlocks_;
};

}  // namespace dock
