#pragma once

#include "Common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

namespace dock {

// Composited per-pixel-alpha rendering surface: D3D11 + DXGI swap chain for
// composition + DirectComposition + Direct2D. Window must be created with
// WS_EX_NOREDIRECTIONBITMAP.
class Surface {
public:
    ~Surface();

    bool Create(HWND hwnd);
    void Destroy();
    bool Resize(UINT width, UINT height);   // pixels; 0 disables drawing
    bool BeginFrame();
    void EndFrame();

    ID2D1DeviceContext* D2D() const { return d2dCtx_; }
    IDWriteFactory* DWrite() const { return dwrite_; }

    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    bool Ready() const { return ready_; }

    void SetDpi(float dpi);

private:
    bool CreateDeviceResources();
    bool CreateSwapChain(UINT width, UINT height);
    void ReleaseTargets();

    HWND hwnd_ = nullptr;

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dCtx_ = nullptr;
    IDXGISwapChain1* swapChain_ = nullptr;
    IDCompositionDevice* compDevice_ = nullptr;
    IDCompositionTarget* compTarget_ = nullptr;
    IDCompositionVisual* compVisual_ = nullptr;

    ID2D1Factory1* d2dFactory_ = nullptr;
    ID2D1Device* d2dDevice_ = nullptr;
    ID2D1DeviceContext* d2dCtx_ = nullptr;
    ID2D1Bitmap1* d2dTarget_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;

    UINT width_ = 0;
    UINT height_ = 0;
    float dpi_ = 96.0f;
    bool ready_ = false;
    bool inFrame_ = false;
};

// Rounded rectangle helper (D2D1 does not ship one before D2D 1.1 geometry
// helpers in this SDK flavour).
ID2D1PathGeometry* MakeRoundRect(ID2D1Factory* factory, const D2D1_RECT_F& r, float radius);
void FillRoundRect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, float radius,
                   const D2D1_COLOR_F& color);
void StrokeRoundRect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, float radius,
                     const D2D1_COLOR_F& color, float width);

}  // namespace dock
