#include "screen.h"
#include "app_identity.h"
#include "capture_region.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace {
constexpr auto windowClass = identity::overlayClass;
constexpr BYTE dimOpacity = 55;
constexpr BYTE selectionOpacity = 150;
constexpr UINT_PTR scanningTimer = 1;
constexpr UINT scanningFrameMs = 300;

void DeleteSurface(HDC& dc, HBITMAP& bitmap, HGDIOBJ& oldBitmap) {
    if (oldBitmap && oldBitmap != HGDI_ERROR) {
        SelectObject(dc, oldBitmap);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    if (dc) {
        DeleteDC(dc);
    }
    dc = nullptr;
    bitmap = nullptr;
    oldBitmap = nullptr;
}
}

CaptureOverlay::~CaptureOverlay() {
    Close();
}

bool CaptureOverlay::Register(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = windowClass;
    return RegisterClassW(&wc) != 0;
}

bool CaptureOverlay::Capture(POINT cursor) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return false;
    }
    bounds_ = monitor.rcMonitor;
    const auto width = bounds_.right - bounds_.left;
    const auto height = bounds_.bottom - bounds_.top;

    HDC desktop = GetDC(nullptr);
    if (!desktop) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* data = nullptr;
    snapshot_ = CreateCompatibleDC(desktop);
    bitmap_ = CreateDIBSection(desktop, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    if (snapshot_ && bitmap_) {
        oldBitmap_ = SelectObject(snapshot_, bitmap_);
    }

    shade_ = CreateCompatibleDC(desktop);
    shadeBitmap_ = CreateCompatibleBitmap(desktop, 2, 1);
    if (shade_ && shadeBitmap_) {
        oldShadeBitmap_ = SelectObject(shade_, shadeBitmap_);
    }
    const bool captured = snapshot_ && oldBitmap_ && oldBitmap_ != HGDI_ERROR &&
        oldShadeBitmap_ && oldShadeBitmap_ != HGDI_ERROR &&
        BitBlt(snapshot_, 0, 0, width, height, desktop, bounds_.left, bounds_.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, desktop);
    if (!captured || !GdiFlush()) {
        return false;
    }

    PrepareSnapshot(static_cast<uint32_t*>(data));
    const auto accent = GetSysColor(COLOR_HIGHLIGHT);
    SetPixelV(shade_, 1, 0, accent);
    return true;
}

void CaptureOverlay::PrepareSnapshot(uint32_t* pixels) {
    const auto width = bounds_.right - bounds_.left;
    const auto height = bounds_.bottom - bounds_.top;
    const auto count = static_cast<size_t>(width) * height;
    ocrImage_ = Image{width, height, std::vector<uint32_t>(pixels, pixels + count)};
    // Keep OCR pixels unchanged; every displayed frame starts with the dimmed DIB.
    constexpr unsigned brightness = 255 - dimOpacity;
    std::transform(pixels, pixels + count, pixels, [](uint32_t pixel) {
        const auto blue = (pixel & 0xff) * brightness / 255;
        const auto green = ((pixel >> 8) & 0xff) * brightness / 255;
        const auto red = ((pixel >> 16) & 0xff) * brightness / 255;
        return (pixel & 0xff000000) | (red << 16) | (green << 8) | blue;
    });
}

bool CaptureOverlay::Open(HINSTANCE instance, HWND owner, UINT copyMessage, POINT cursor,
                          UINT scanRegionMessage, UINT cancelRegionMessage) {
    if (!Capture(cursor)) {
        Close();
        return false;
    }

    owner_ = owner;
    copyMessage_ = copyMessage;
    scanRegionMessage_ = scanRegionMessage;
    cancelRegionMessage_ = cancelRegionMessage;
    previousForeground_ = GetForegroundWindow();
    const auto window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, windowClass, identity::appName,
        WS_POPUP, bounds_.left, bounds_.top, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top,
        owner, nullptr, instance, this);
    if (!window || !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
        Close();
        return false;
    }
    SetForegroundWindow(window);
    SetFocus(window);
    if (window_ != window) {
        return false;
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return window_ == window;
}

void CaptureOverlay::Close(bool restoreFocus) {
    if (window_ && owner_ && cancelRegionMessage_) {
        SendMessageW(owner_, cancelRegionMessage_, 0, 0);
    }
    dragging_ = false;
    StopScanning();
    if (const auto window = std::exchange(window_, nullptr)) {
        if (restoreFocus && previousForeground_ && GetForegroundWindow() == window) {
            SetForegroundWindow(previousForeground_);
        }
        DestroyWindow(window);
    }
    previousForeground_ = nullptr;
    ClearSelection();
    layout_.reset();
    ocrImage_ = {};
    DeleteSurface(shade_, shadeBitmap_, oldShadeBitmap_);
    DeleteSurface(snapshot_, bitmap_, oldBitmap_);
    if (scanningFont_) {
        DeleteObject(std::exchange(scanningFont_, nullptr));
    }
}

void CaptureOverlay::ClearSelection() {
    InvalidateFrame();
    if (window_) {
        for (const auto& highlight : selection_.highlights) {
            InvalidateRect(window_, &highlight, FALSE);
        }
    }
    anchor_ = {};
    cursor_ = {};
    selection_ = {};
    copyWhenReady_ = false;
}

void CaptureOverlay::MoveSelection(POINT point) {
    point.x = std::clamp(point.x, 0L, bounds_.right - bounds_.left);
    point.y = std::clamp(point.y, 0L, bounds_.bottom - bounds_.top);
    if (point.x == cursor_.x && point.y == cursor_.y) {
        return;
    }
    InvalidateFrame();
    cursor_ = point;
    ++gesture_;
    InvalidateFrame();
    UpdateWindow(window_);
}

void CaptureOverlay::UpdateSelection() {
    if (!layout_) {
        return;
    }
    for (const auto& highlight : selection_.highlights) {
        InvalidateRect(window_, &highlight, FALSE);
    }
    selection_ = layout_->Select({-1, -1},
        {bounds_.right - bounds_.left + 1, bounds_.bottom - bounds_.top + 1});
    for (const auto& highlight : selection_.highlights) {
        InvalidateRect(window_, &highlight, FALSE);
    }
    UpdateWindow(window_);
}

Image CaptureOverlay::SnapshotForOcr() {
    return CropForOcr(ocrImage_, Frame());
}

RECT CaptureOverlay::Frame() const {
    return CaptureRegion(anchor_, cursor_, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top);
}

void CaptureOverlay::InvalidateFrame() {
    if (!window_) return;
    auto area = Frame();
    if (!IsRectEmpty(&area)) {
        InflateRect(&area, 3, 3);
        InvalidateRect(window_, &area, FALSE);
    }
}

void CaptureOverlay::ScanRegion() {
    const auto region = Frame();
    if (region.right - region.left < 2 || region.bottom - region.top < 2) {
        if (std::exchange(copyWhenReady_, false)) Close(true);
        return;
    }
    StartScanning();
    InvalidateRect(window_, &scanningBounds_, FALSE);
    UpdateWindow(window_);
    PostMessageW(owner_, scanRegionMessage_, reinterpret_cast<WPARAM>(window_), static_cast<LPARAM>(gesture_));
}

void CaptureOverlay::SetText(TextLayout layout) {
    layout_ = std::move(layout);
    StopScanning();
    UpdateSelection();
    if (!dragging_ && std::exchange(copyWhenReady_, false)) {
        RequestCopy();
    }
}

std::wstring_view CaptureOverlay::SelectedText(HWND source, size_t gesture) const {
    return window_ == source && IsCurrent(gesture) ? std::wstring_view(selection_.text) : std::wstring_view{};
}

void CaptureOverlay::RequestCopy() {
    if (!dragging_ && !scanningVisible_ &&
        (!layout_ || selection_.text.empty())) {
        Close(true);
        return;
    }
    if (!layout_ || dragging_) {
        copyWhenReady_ = true;
    } else if (!selection_.text.empty()) {
        PostMessageW(owner_, copyMessage_, reinterpret_cast<WPARAM>(window_), static_cast<LPARAM>(gesture_));
    }
}

LRESULT CALLBACK CaptureOverlay::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CaptureOverlay*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    try {
        return self->HandleMessage(window, message, wParam, lParam);
    } catch (...) {
        self->Close();
        return 0;
    }
}

LRESULT CaptureOverlay::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint(window);
        return 0;
    case WM_TIMER:
        if (wParam == scanningTimer && scanningVisible_ && !layout_) {
            scanningDots_ = scanningVisible_ ? scanningDots_ % 3 + 1 : 1;
            scanningVisible_ = true;
            SetTimer(window, scanningTimer, scanningFrameMs, nullptr);
            InvalidateRect(window, &scanningBounds_, FALSE);
            UpdateWindow(window);
        }
        return 0;
    case WM_LBUTTONDOWN:
        ++gesture_;
        ClearSelection();
        StopScanning();
        layout_.reset();
        SendMessageW(owner_, cancelRegionMessage_, 0, 0);
        SetCapture(window);
        dragging_ = GetCapture() == window;
        if (dragging_) {
            anchor_ = cursor_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        }
        UpdateWindow(window);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) {
            MoveSelection({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            MoveSelection({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            dragging_ = false;
            ReleaseCapture();
            ScanRegion();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_ESCAPE) {
            Close(true);
            return 0;
        }
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000)) {
            RequestCopy();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && window_) {
            Close();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (std::exchange(dragging_, false)) {
            ClearSelection();
            UpdateWindow(window);
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DPICHANGED:
        // Capture and selection use physical monitor pixels.
        return 0;
    case WM_NCDESTROY:
        window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void CaptureOverlay::Paint(HWND window) const {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    Draw(dc, paint.rcPaint);
    EndPaint(window, &paint);
}

void CaptureOverlay::Draw(HDC dc, const RECT& dirty) const {
    if (snapshot_ && !IsRectEmpty(&dirty)) {
        const auto width = dirty.right - dirty.left;
        const auto height = dirty.bottom - dirty.top;
        BitBlt(dc, dirty.left, dirty.top, width, height, snapshot_, dirty.left, dirty.top, SRCCOPY);

        for (const auto& highlight : selection_.highlights) {
            RECT clipped{};
            if (IntersectRect(&clipped, &dirty, &highlight)) {
                AlphaBlend(dc, clipped.left, clipped.top, clipped.right - clipped.left,
                    clipped.bottom - clipped.top, shade_, 1, 0, 1, 1,
                    BLENDFUNCTION{AC_SRC_OVER, 0, selectionOpacity, 0});
            }
        }
        auto frame = Frame();
        if (!IsRectEmpty(&frame)) {
            const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, RGB(10, 10, 10));
            FrameRect(dc, &frame, brush);
            InflateRect(&frame, -1, -1);
            SetDCBrushColor(dc, RGB(35, 170, 255));
            FrameRect(dc, &frame, brush);
        }
        RECT clipped{};
        if (scanningVisible_ && IntersectRect(&clipped, &dirty, &scanningBounds_)) {
            DrawScanning(dc);
        }
    }
}

void CaptureOverlay::StartScanning() {
    const auto dpi = static_cast<int>(GetDpiForWindow(window_));
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const auto width = scale(240);
    const auto height = scale(64);
    const auto left = (bounds_.right - bounds_.left - width) / 2;
    const auto top = (bounds_.bottom - bounds_.top - height) / 2;
    scanningBounds_ = {left, top, left + width, top + height};
    if (!scanningFont_) {
        scanningFont_ = CreateFontW(-scale(28), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
    }
    scanningVisible_ = true;
    scanningDots_ = 1;
    SetTimer(window_, scanningTimer, scanningFrameMs, nullptr);
}

void CaptureOverlay::StopScanning() {
    if (window_) {
        KillTimer(window_, scanningTimer);
        if (scanningVisible_) {
            InvalidateRect(window_, &scanningBounds_, FALSE);
        }
    }
    scanningVisible_ = false;
    scanningDots_ = 0;
}

void CaptureOverlay::DrawScanning(HDC dc) const {
    const auto saved = SaveDC(dc);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, RGB(32, 32, 32));
    SetDCPenColor(dc, RGB(65, 65, 65));
    const auto height = scanningBounds_.bottom - scanningBounds_.top;
    RoundRect(dc, scanningBounds_.left, scanningBounds_.top,
        scanningBounds_.right, scanningBounds_.bottom, height / 2, height / 2);
    SelectObject(dc, scanningFont_ ? scanningFont_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SIZE size{};
    // Reserve the full label width so the word stays still as the dots animate.
    constexpr wchar_t label[] = L"Scanning...";
    GetTextExtentPoint32W(dc, label, 11, &size);
    TextOutW(dc, (scanningBounds_.left + scanningBounds_.right - size.cx) / 2,
        scanningBounds_.top + (height - size.cy) / 2, label, 8 + scanningDots_);
    RestoreDC(dc, saved);
}
