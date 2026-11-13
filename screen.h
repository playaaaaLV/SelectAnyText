#pragma once

#include "text_layout.h"

#include <cstdint>
#include <optional>
#include <string_view>

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    POINT origin{};
};

class CaptureOverlay {
public:
    CaptureOverlay() = default;
    ~CaptureOverlay();
    CaptureOverlay(const CaptureOverlay&) = delete;
    CaptureOverlay& operator=(const CaptureOverlay&) = delete;

    static bool Register(HINSTANCE instance);
    bool Open(HINSTANCE instance, HWND owner, UINT copyMessage, POINT cursor,
              UINT scanRegionMessage, UINT cancelRegionMessage);
    void Close(bool restoreFocus = false);
    bool IsOpen() const { return window_ != nullptr; }
    bool IsCurrent(size_t gesture) const { return window_ && gesture_ == gesture; }

    Image SnapshotForOcr();
    void SetText(TextLayout layout);
    std::wstring_view SelectedText(HWND source, size_t gesture) const;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool Capture(POINT cursor);
    void PrepareSnapshot(uint32_t* pixels);
    void Paint(HWND window) const;
    void Draw(HDC dc, const RECT& dirty) const;
    void StartScanning();
    void StopScanning();
    void DrawScanning(HDC dc) const;
    void ClearSelection();
    void MoveSelection(POINT point);
    void UpdateSelection();
    void RequestCopy();
    RECT Frame() const;
    void InvalidateFrame();
    void ScanRegion();

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND previousForeground_ = nullptr;
    UINT copyMessage_ = 0;
    UINT scanRegionMessage_ = 0;
    UINT cancelRegionMessage_ = 0;
    RECT bounds_{};
    HDC snapshot_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    Image ocrImage_;
    HDC shade_ = nullptr;
    HBITMAP shadeBitmap_ = nullptr;
    HGDIOBJ oldShadeBitmap_ = nullptr;
    POINT anchor_{};
    POINT cursor_{};
    std::optional<TextLayout> layout_;
    RegionSelection selection_;
    size_t gesture_ = 0;
    bool dragging_ = false;
    bool copyWhenReady_ = false;
    HFONT scanningFont_ = nullptr;
    RECT scanningBounds_{};
    unsigned scanningDots_ = 0;
    bool scanningVisible_ = false;
};
