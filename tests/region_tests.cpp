#include "app_identity.h"
#include "screen.h"

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

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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
        Check(RegisterClassW(&ownerClass) != 0, "Owner registration failed");
        const auto owner = CreateWindowW(ownerClass.lpszClassName, L"", 0, 0, 0, 0, 0,
            nullptr, nullptr, instance, nullptr);
        Check(owner != nullptr, "Owner creation failed");
        Check(CaptureOverlay::Register(instance), "Overlay registration failed");
        CaptureOverlay capture;
        overlay = &capture;
        const auto open = [&] {
            Check(capture.Open(instance, owner, copyMessage, {100,100}, scanMessage, cancelMessage), "Open failed");
            Pump();
            const auto window = FindWindowW(identity::overlayClass, nullptr);
            Check(window != nullptr, "Overlay missing");
            return window;
        };

        auto window = open();
        Check(scans == 0, "Opening the overlay must not start full-screen OCR");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100,100));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(360,220));
        Pump();
        Check(scans == 0, "Dragging must not start OCR before release");
        const auto dc = GetDC(window);
        const auto border = GetPixel(dc, 101, 110);
        ReleaseDC(window, dc);
        Check(border == RGB(35,170,255), "The region frame is not visibly painted");
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(360,220));
        Pump();
        Check(scans == 1 && crop.width == 260 && crop.height == 120 &&
            crop.origin.x == 100 && crop.origin.y == 100, "OCR received the wrong region");
        Check(capture.IsCurrent(gesture), "Submitted gesture was invalidated");
        capture.SetText(TextLayout::FromFragments({Word(L"First",110,110), Word(L"line",180,110),
            Word(L"Second",110,160)}));
        Check(capture.SelectedText(window, gesture) == L"First line\r\nSecond", "All crop text must be selected");

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
        std::cout << "Region checks passed: visible frame, release-to-scan, crop pixels, select-all, repeat/reverse, Ctrl+C, Esc\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
