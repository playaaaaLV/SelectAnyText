#include "app_identity.h"
#include "screen.h"

#include <dwmapi.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
constexpr UINT copyMessage = WM_APP + 20;
constexpr UINT scanMessage = WM_APP + 21;
constexpr UINT cancelMessage = WM_APP + 22;
CaptureOverlay* overlay = nullptr;
int scans = 0;
int cancellations = 0;
size_t gesture = 0;
Image crop;
std::wstring copied;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == scanMessage) {
        ++scans;
        gesture = static_cast<size_t>(lParam);
        crop = overlay->SnapshotForOcr();
        return 0;
    }
    if (message == cancelMessage) {
        ++cancellations;
        return 0;
    }
    if (message == copyMessage) {
        copied = overlay->SelectedText(reinterpret_cast<HWND>(wParam), static_cast<size_t>(lParam));
        overlay->Close(true);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void DispatchTestMessage(const MSG& message) {
    // Drag() sends positions synchronously. Windows also queues a move at
    // the user's actual cursor when SetCapture runs; that is a different gesture.
    if (message.message == WM_MOUSEMOVE) return;
    TranslateMessage(&message);
    DispatchMessageW(&message);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        DispatchTestMessage(message);
    }
}

BYTE BrightnessAt(HWND window, int x, int y) {
    const auto dc = GetDC(window);
    const auto color = GetPixel(dc, x, y);
    ReleaseDC(window, dc);
    Check(color != CLR_INVALID, "Could not sample overlay brightness");
    return GetRValue(color);
}

TextFragment Word(std::wstring text, LONG left, LONG top) {
    const RECT rect{left, top, left + static_cast<LONG>(text.size()) * 10, top + 20};
    TextWord word;
    word.bounds = rect;
    for (size_t i = 0; i <= text.size(); ++i) word.stops.push_back({static_cast<float>(left + i * 10), i});
    return {{rect, {std::move(word)}}, std::move(text)};
}

void CtrlC(HWND window) {
    BYTE saved[256]{};
    BYTE keys[256]{};
    GetKeyboardState(saved);
    keys[VK_CONTROL] = 0x80;
    keys[VK_LCONTROL] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(window, WM_KEYDOWN, 'C', 0);
    SetKeyboardState(saved);
    Pump();
}

void Drag(HWND window, POINT from, POINT to) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
    Pump();
}
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW ownerClass{};
        ownerClass.hInstance = instance;
        ownerClass.lpfnWndProc = OwnerProc;
        ownerClass.lpszClassName = L"SelectAnyText.Tests.RegionOwner";
        ownerClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        Check(RegisterClassW(&ownerClass) != 0, "Owner registration failed");
        const auto owner = CreateWindowW(ownerClass.lpszClassName, L"", WS_POPUP, 0, 0, 480, 300,
            nullptr, nullptr, instance, nullptr);
        Check(owner != nullptr, "Owner creation failed");
        Check(SetWindowPos(owner, HWND_TOPMOST, 0, 0, 480, 300, SWP_NOACTIVATE | SWP_SHOWWINDOW),
            "Test background could not be shown");
        UpdateWindow(owner);
        DwmFlush();
        Check(CaptureOverlay::Register(instance), "Overlay registration failed");
        CaptureOverlay capture;
        overlay = &capture;
        const auto open = [&] {
            Check(capture.Open(instance, owner, copyMessage, {100,100}, scanMessage, cancelMessage), "Open failed");
            const auto window = FindWindowW(identity::overlayClass, nullptr);
            Check(window != nullptr, "Overlay missing");
            return window;
        };

        auto window = open();
        Check(scans == 0, "Opening the overlay must not start full-screen OCR");
        // Compare against the actual capture; some desktops omit test-owned windows.
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0,0));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(480,300));
        const auto original = capture.SnapshotForOcr();
        Check(original.width == 480 && original.height == 300, "Missing reference capture");
        const auto sourceRed = [&](int x, int y) {
            return static_cast<int>((original.pixels[static_cast<size_t>(y) * original.width + x] >> 16) & 0xff);
        };
        const auto finalOutside = sourceRed(50,150) * 165 / 255;
        const auto initialBrightness = BrightnessAt(window, 50, 150);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100,100));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(360,220));
        Check(scans == 0, "Dragging must not start OCR before release");
        auto previousBrightness = initialBrightness;
        bool intermediateFrame = false;
        std::vector<unsigned> fadeSamples{initialBrightness};
        const auto fadeDeadline = GetTickCount64() + 300;
        do {
            // Observe each painted timer frame instead of draining the entire fade.
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                DispatchTestMessage(message);
            }
            const auto brightness = BrightnessAt(window, 50, 150);
            Check(brightness <= previousBrightness, "Dimming must not flash brighter during the fade");
            if (brightness != previousBrightness) fadeSamples.push_back(brightness);
            intermediateFrame |= brightness < initialBrightness - 1 && brightness > finalOutside + 1;
            previousBrightness = brightness;
            Sleep(4);
        } while (GetTickCount64() < fadeDeadline);
        Pump();
        const bool startedClear = initialBrightness + 2 >= sourceRed(50,150);
        if (!(startedClear && intermediateFrame)) {
            std::cerr << "Fade samples:";
            for (auto value : fadeSamples) std::cerr << ' ' << value;
            std::cerr << '\n';
        }
        Check(startedClear && intermediateFrame,
            "Dimming must start clear and display intermediate frames while dragging");
        const auto dc = GetDC(window);
        const auto border = GetPixel(dc, 101, 110);
        const auto inside = GetPixel(dc, 150, 150);
        const auto outside = GetPixel(dc, 50, 150);
        ReleaseDC(window, dc);
        Check(border == RGB(35,170,255), "The region frame is not visibly painted");
        const auto closeEnough = [](int actual, int expected) { return std::abs(actual - expected) <= 2; };
        Check(closeEnough(GetRValue(inside), sourceRed(150,150) * 200 / 255) &&
            closeEnough(GetRValue(outside), finalOutside),
            "The surroundings must be darker without further dimming the crop");
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(240,220));
        Pump();
        const auto resizedDc = GetDC(window);
        const auto vacated = GetPixel(resizedDc, 300, 150);
        ReleaseDC(window, resizedDc);
        Check(closeEnough(GetRValue(vacated), sourceRed(300,150) * 165 / 255),
            "Shrinking the frame must restore the darker surroundings");
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(360,220));
        Pump();
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(360,220));
        Pump();
        Check(scans == 1 && crop.width == 260 && crop.height == 120 &&
            crop.origin.x == 100 && crop.origin.y == 100, "OCR received the wrong region");
        Check(!crop.pixels.empty() && crop.pixels.front() == original.pixels[100 * original.width + 100],
            "Display dimming must not change the source OCR pixels");
        Check(capture.IsCurrent(gesture), "Submitted gesture was invalidated");
        capture.SetText(TextLayout::FromFragments({Word(L"First",110,110), Word(L"line",180,110),
            Word(L"Second",110,160)}));
        Check(capture.SelectedText(window, gesture) == L"First line\r\nSecond", "All crop text must be selected");

        capture.SetText({});
        const auto sample = [&](int x, int y) {
            auto surface = GetDC(window);
            auto color = GetPixel(surface, x, y);
            ReleaseDC(window, surface);
            return color;
        };
        const auto bandBefore = sample(206,157), cornerBefore = sample(237,132);
        capture.SetText(TextLayout::FromFragments({Word(L"Diagonal",0,0)}, {180,125,.8,.6}));
        Pump();
        // Both samples are inside the axis-aligned bounds, only the first is on the letters.
        Check(bandBefore != CLR_INVALID && cornerBefore != CLR_INVALID &&
              sample(206,157) != bandBefore && sample(237,132) == cornerBefore,
              "The overlay must paint only the inclined polygon");

        // A new rectangle replaces the previous result and retains source pixels.
        const auto savedPixels = crop.pixels;
        Drag(window, {360,220}, {100,100});
        Check(scans == 2 && crop.pixels == savedPixels, "Reverse/repeated drag changed the source crop");
        Check(capture.SelectedText(window, gesture).empty(), "Previous result survived a new rectangle");
        capture.SetText(TextLayout::FromFragments({Word(L"Replacement",110,110)}));
        CtrlC(window);
        Check(copied == L"Replacement" && !capture.IsOpen(), "Ctrl+C must copy the latest result and close");

        copied.clear();
        window = open();
        Drag(window, {100,100}, {360,220});
        CtrlC(window);
        Check(capture.IsOpen() && copied.empty(), "Ctrl+C during OCR copied an incomplete result");
        capture.SetText(TextLayout::FromFragments({Word(L"Completed",110,110)}));
        Pump();
        Check(copied == L"Completed" && !capture.IsOpen(), "Queued copy did not finish after OCR");

        window = open();
        const auto beforeClick = scans;
        Drag(window, {120,120}, {120,120});
        Check(scans == beforeClick, "An empty click must not scan");
        CtrlC(window);
        Check(!capture.IsOpen(), "Ctrl+C with no region must close");

        window = open();
        Drag(window, {100,100}, {360,220});
        capture.SetText(TextLayout{});
        CtrlC(window);
        Check(!capture.IsOpen(), "Ctrl+C after an empty scan must close");

        window = open();
        Drag(window, {100,100}, {360,220});
        const auto beforeCancel = cancellations;
        SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        Pump();
        Check(!capture.IsOpen() && cancellations > beforeCancel, "Esc must close and cancel OCR");
        DestroyWindow(owner);
        overlay = nullptr;
        std::cout << "Region checks passed: animated dimming, visible frame, outside dimming, release-to-scan, crop pixels, select-all, repeat/reverse, Ctrl+C, Esc\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
