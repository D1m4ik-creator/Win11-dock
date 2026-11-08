#include "Surface.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

namespace dock {

namespace {
template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
}  // namespace

Surface::~Surface() { Destroy(); }

bool Surface::Create(HWND hwnd) {
    hwnd_ = hwnd;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 nullptr, reinterpret_cast<void**>(&d2dFactory_)))) {
        return false;
    }

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&dwrite_));

    if (!CreateDeviceResources()) return false;

    if (FAILED(DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                        reinterpret_cast<void**>(&compDevice_)))) {
        return false;
    }
    if (FAILED(compDevice_->CreateTargetForHwnd(hwnd_, TRUE, &compTarget_))) return false;
    if (FAILED(compDevice_->CreateVisual(&compVisual_))) return false;

    ready_ = true;
    return true;
}

bool Surface::CreateDeviceResources() {
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDevice_, &got,
                                   &d3dCtx_);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDevice_, &got, &d3dCtx_);
        if (FAILED(hr)) return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
    hr = d2dFactory_->CreateDevice(dxgiDevice, &d2dDevice_);
    dxgiDevice->Release();
    if (FAILED(hr)) return false;

    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_))) {
        return false;
    }

    d2dCtx_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    d2dCtx_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

void Surface::ReleaseTargets() {
    if (d2dCtx_) d2dCtx_->SetTarget(nullptr);
    SafeRelease(d2dTarget_);
    SafeRelease(swapChain_);
}

bool Surface::CreateSwapChain(UINT width, UINT height) {
    ReleaseTargets();
    if (width == 0 || height == 0) return true;

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;

    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    bool ok = false;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling = DXGI_SCALING_STRETCH;

        if (SUCCEEDED(factory->CreateSwapChainForComposition(d3dDevice_, &desc, nullptr,
                                                             &swapChain_))) {
            IDXGISurface* surface = nullptr;
            if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    dpi_, dpi_);
                if (SUCCEEDED(d2dCtx_->CreateBitmapFromDxgiSurface(surface, &props, &d2dTarget_))) {
                    d2dCtx_->SetTarget(d2dTarget_);
                    ok = true;
                }
                surface->Release();
            }
        }
    }
    SafeRelease(factory);
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);

    if (!ok) {
        ReleaseTargets();
        return false;
    }

    // (Re)wire the composition tree: root visual -> swap chain content.
    if (compVisual_ && compTarget_) {
        compVisual_->SetContent(swapChain_);
        compTarget_->SetRoot(compVisual_);
        compDevice_->Commit();
    }

    width_ = width;
    height_ = height;
    return true;
}

void Surface::SetDpi(float dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi;
    if (d2dCtx_) d2dCtx_->SetDpi(dpi_, dpi_);
    if (width_ && height_) {
        const UINT w = width_, h = height_;
        ReleaseTargets();
        CreateSwapChain(w, h);
    }
}

bool Surface::Resize(UINT width, UINT height) {
    if (width == width_ && height == height_ && (d2dTarget_ || width == 0)) return true;
    if (width == 0 || height == 0) {
        width_ = width;
        height_ = height;
        ReleaseTargets();
        return true;
    }
    return CreateSwapChain(width, height);
}

bool Surface::BeginFrame() {
    if (!ready_ || !d2dTarget_) return false;
    d2dCtx_->BeginDraw();
    d2dCtx_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2dCtx_->Clear(D2D1::ColorF(0, 0.0f));
    inFrame_ = true;
    return true;
}

void Surface::EndFrame() {
    if (!inFrame_) return;
    const HRESULT hr = d2dCtx_->EndDraw();
    inFrame_ = false;
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseTargets();
        CreateSwapChain(width_, height_);
        return;
    }
    swapChain_->Present(0, 0);
}

void Surface::Destroy() {
    ReleaseTargets();
    SafeRelease(compVisual_);
    SafeRelease(compTarget_);
    SafeRelease(compDevice_);
    SafeRelease(d2dCtx_);
    SafeRelease(d2dDevice_);
    SafeRelease(d2dFactory_);
    SafeRelease(dwrite_);
    SafeRelease(d3dCtx_);
    SafeRelease(d3dDevice_);
    ready_ = false;
    hwnd_ = nullptr;
}

ID2D1PathGeometry* MakeRoundRect(ID2D1Factory* factory, const D2D1_RECT_F& r, float radius) {
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geo))) return nullptr;

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink))) {
        geo->Release();
        return nullptr;
    }

    const float maxR = std::min(r.right - r.left, r.bottom - r.top) * 0.5f;
    const float rad = std::max(0.0f, std::min(radius, maxR));
    const bool straight = rad <= 0.01f;

    sink->BeginFigure(D2D1::Point2F(r.left + rad, r.top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(r.right - rad, r.top));
    if (!straight) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right, r.top + rad),
                                      D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->AddLine(D2D1::Point2F(r.right, r.bottom - rad));
    if (!straight) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.right - rad, r.bottom),
                                      D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->AddLine(D2D1::Point2F(r.left + rad, r.bottom));
    if (!straight) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left, r.bottom - rad),
                                      D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->AddLine(D2D1::Point2F(r.left, r.top + rad));
    if (!straight) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r.left + rad, r.top),
                                      D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    return geo;
}

void FillRoundRect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, float radius,
                   const D2D1_COLOR_F& color) {
    ID2D1Factory* factory = nullptr;
    ctx->GetFactory(&factory);
    if (!factory) return;
    ID2D1PathGeometry* geo = MakeRoundRect(factory, r, radius);
    if (!geo) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush))) {
        ctx->FillGeometry(geo, brush);
        brush->Release();
    }
    geo->Release();
    factory->Release();
}

void StrokeRoundRect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r, float radius,
                     const D2D1_COLOR_F& color, float width) {
    ID2D1Factory* factory = nullptr;
    ctx->GetFactory(&factory);
    if (!factory) return;
    ID2D1PathGeometry* geo = MakeRoundRect(factory, r, radius);
    if (!geo) return;

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush))) {
        ctx->DrawGeometry(geo, brush, width);
        brush->Release();
    }
    geo->Release();
    factory->Release();
}

}  // namespace dock
