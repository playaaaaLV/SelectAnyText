#include "resource.h"
#include "screen.h"
#include "settings.h"
#include "settings_theme.h"
#include "transfer.h"

#include "app_identity.h"
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <utility>

namespace {
constexpr auto appName = identity::appName;
constexpr auto windowClass = identity::trayClass;
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT copyMessage = WM_APP + 2;
constexpr UINT ocrMessage = WM_APP + 3;
constexpr UINT showSettingsMessage = WM_APP + 5;
constexpr UINT shortcutCapturedMessage = WM_APP + 6;
constexpr UINT scanRegionMessage = WM_APP + 7;
constexpr UINT cancelRegionMessage = WM_APP + 8;
constexpr UINT trayId = 1;
constexpr UINT menuSettings = 1;
constexpr UINT menuQuit = 2;
constexpr auto hotkeyError = L"Shortcut is in use or unavailable.";
constexpr std::array modifierKeys{
    VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN,
    VK_CONTROL, VK_MENU, VK_SHIFT,
};

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    int Run();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void AddTrayIcon();
    void Notify(const wchar_t* status) const;
    void TrayMenu(POINT point);
    void ShowSettings();
    void StartShortcutCapture();
    void StopShortcutCapture();
    void SetShortcut(Settings shortcut);
    bool CaptureKey(UINT key, bool down);
    void SaveSettings();
    void SetStatus(const wchar_t* status) const;
    void BeginCapture();
    void BeginRegionScan(HWND source, size_t gesture);
    void CopySelection(HWND source, size_t gesture);
    void CopyToClipboard(std::wstring_view text);
    void FinishOcr();

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND dialog_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    int hotkeyId_ = 0;
    Settings settings_ = ReadSettings();
    Settings shortcutDraft_;
    SettingsTheme settingsTheme_;
    HHOOK keyboardHook_ = nullptr;
    std::array<bool, 256> pressed_{};
    std::array<bool, 256> intercepted_{};
    inline static App* shortcutOwner_ = nullptr;
    CaptureOverlay overlay_;
    size_t captureId_ = 0;
    size_t ocrGesture_ = 0;
    std::optional<OcrWorker> worker_;
};

int App::Run() {
    icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hIcon = icon_;
    wc.lpszClassName = windowClass;
    if (!RegisterClassW(&wc) || !CaptureOverlay::Register(instance_)) {
        return 1;
    }

    if (!CreateWindowExW(0, windowClass, appName, WS_OVERLAPPED, 0, 0, 0, 0,
            nullptr, nullptr, instance_, this)) {
        return 1;
    }
    AddTrayIcon();
    // Load the OCR models in the background before the first capture.
    worker_.emplace(window_, ocrMessage);
    if (RegisterHotKey(window_, 1, settings_.modifiers | MOD_NOREPEAT, settings_.key)) {
        hotkeyId_ = 1;
    } else {
        Notify((FormatShortcut(settings_) + L" is in use or unavailable.").c_str());
    }

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!dialog_ || !IsDialogMessageW(dialog_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (result == -1) {
        DestroyWindow(window_);
        return 1;
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->HandleMessage(message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated_ && message == taskbarCreated_) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
    case trayMessage:
        if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT ||
            LOWORD(lParam) == WM_LBUTTONUP) {
            ShowSettings();
        } else if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            TrayMenu(point);
        }
        return 0;
    case WM_HOTKEY:
        if (hotkeyId_ && wParam == static_cast<WPARAM>(hotkeyId_)) {
            if (dialog_ && GetForegroundWindow() == dialog_) {
                SetShortcut(settings_);
            } else if (!overlay_.IsOpen()) {
                BeginCapture();
            }
        }
        return 0;
    case showSettingsMessage:
        ShowSettings();
        return 0;
    case copyMessage:
        CopySelection(reinterpret_cast<HWND>(wParam), static_cast<size_t>(lParam));
        return 0;
    case ocrMessage:
        FinishOcr();
        return 0;
    case scanRegionMessage:
        BeginRegionScan(reinterpret_cast<HWND>(wParam), static_cast<size_t>(lParam));
        return 0;
    case cancelRegionMessage:
        ++captureId_;
        if (worker_) worker_->Cancel();
        return 0;
    case WM_DESTROY: {
        overlay_.Close();
        if (dialog_) {
            DestroyWindow(dialog_);
        }
        worker_.reset();
        if (hotkeyId_) {
            UnregisterHotKey(window_, hotkeyId_);
        }
        NOTIFYICONDATAW icon{sizeof(NOTIFYICONDATAW), window_, trayId};
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void App::AddTrayIcon() {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW), window_, trayId};
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = trayMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, appName);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void App::Notify(const wchar_t* status) const {
    NOTIFYICONDATAW data{sizeof(NOTIFYICONDATAW), window_, trayId};
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    wcscpy_s(data.szInfoTitle, appName);
    wcscpy_s(data.szInfo, status);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::TrayMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, menuSettings, L"Settings");
    AppendMenuW(menu, MF_STRING, menuQuit, L"Exit");
    SetForegroundWindow(window_);
    const auto command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == menuSettings) {
        ShowSettings();
    } else if (command == menuQuit) {
        DestroyWindow(window_);
    }
}

void App::ShowSettings() {
    overlay_.Close();
    if (!dialog_) {
        CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
            DialogProc, reinterpret_cast<LPARAM>(this));
    }
    const auto dialog = dialog_;
    if (!dialog) {
        Notify(L"Could not open Settings.");
        return;
    }
    if (IsIconic(dialog)) {
        ShowWindow(dialog, SW_RESTORE);
    } else {
        SetWindowPos(dialog, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    SetForegroundWindow(dialog);
}

INT_PTR CALLBACK App::DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        app = reinterpret_cast<App*>(lParam);
        app->dialog_ = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(app));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app->icon_));
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(
            LoadIconW(app->instance_, MAKEINTRESOURCEW(IDI_APP))));
        app->settingsTheme_.Update(dialog);
        app->SetShortcut(app->settings_);
        if (!app->hotkeyId_) {
            app->SetStatus(hotkeyError);
        }
        app->StartShortcutCapture();
        return TRUE;
    }
    if (!app) {
        return FALSE;
    }
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            app->SaveSettings();
            return TRUE;
        case IDCANCEL:
            DestroyWindow(dialog);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;
    case WM_NCDESTROY:
        app->StopShortcutCapture();
        app->dialog_ = nullptr;
        break;
    case shortcutCapturedMessage:
        app->SetShortcut({static_cast<UINT>(wParam), static_cast<UINT>(lParam)});
        return TRUE;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_SYSCOLORCHANGE:
        app->settingsTheme_.Update(dialog);
        break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return app->settingsTheme_.Color(reinterpret_cast<HDC>(wParam),
            wParam && message == WM_CTLCOLORSTATIC ? reinterpret_cast<HWND>(lParam) : nullptr);
    case WM_DRAWITEM:
        return app->settingsTheme_.Draw(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
    }
    return FALSE;
}

void App::StartShortcutCapture() {
    pressed_.fill(false);
    intercepted_.fill(false);
    for (const auto key : modifierKeys) {
        if (key != VK_CONTROL && key != VK_MENU && key != VK_SHIFT) {
            pressed_[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
        }
    }
    shortcutOwner_ = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance_, 0);
    if (!keyboardHook_) {
        shortcutOwner_ = nullptr;
        SetStatus(L"Shortcut capture is unavailable.");
    }
}

void App::StopShortcutCapture() {
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    shortcutOwner_ = nullptr;
}

LRESULT CALLBACK App::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && shortcutOwner_) {
        const auto key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode;
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        if (shortcutOwner_->CaptureKey(key, down)) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool App::CaptureKey(UINT key, bool down) {
    if (key >= pressed_.size()) {
        return false;
    }
    const bool focused = dialog_ && GetForegroundWindow() == dialog_;
    const auto modifier = ShortcutModifier(key);
    if (modifier) {
        const bool alreadyDown = pressed_[key];
        pressed_[key] = down;
        if (down && focused && !alreadyDown) {
            intercepted_[key] = true;
        }
    }
    if (!down) {
        return std::exchange(intercepted_[key], false);
    }
    if (intercepted_[key]) {
        return true;
    }
    if (!focused || modifier) {
        return false;
    }
    UINT modifiers = 0;
    for (const auto modifierKey : modifierKeys) {
        if (pressed_[modifierKey]) {
            modifiers |= ShortcutModifier(modifierKey);
        }
    }
    if (!IsValidShortcut({modifiers, key})) {
        return false;
    }
    intercepted_[key] = PostMessageW(dialog_, shortcutCapturedMessage, modifiers, key) != FALSE;
    return intercepted_[key];
}

void App::SetShortcut(Settings shortcut) {
    if (IsValidShortcut(shortcut)) {
        shortcutDraft_ = shortcut;
        const auto control = GetDlgItem(dialog_, IDC_SHORTCUT);
        SetWindowTextW(control, FormatShortcut(shortcut).c_str());
        InvalidateRect(control, nullptr, FALSE);
        SetStatus(nullptr);
    }
}

void App::SetStatus(const wchar_t* status) const {
    SetDlgItemTextW(dialog_, IDC_STATUS, status ? status : L"");
    const auto control = GetDlgItem(dialog_, IDC_STATUS);
    const bool visible = status && *status;
    RECT size{0, 0, 164, 64};
    RECT button{54, 43, 110, 59};
    RECT message{10, 43, 154, 53};
    MapDialogRect(dialog_, &size);
    MapDialogRect(dialog_, &button);
    MapDialogRect(dialog_, &message);
    if (visible) {
        const auto dc = GetDC(control);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        const auto old = font ? SelectObject(dc, font) : nullptr;
        RECT text{0, 0, message.right - message.left, 0};
        DrawTextW(dc, status, -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
        ReleaseDC(control, dc);
        const auto height = std::max(message.bottom - message.top, text.bottom);
        SetWindowPos(control, nullptr, message.left, message.top, message.right - message.left,
            height, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto extra = height + MulDiv(8, GetDpiForWindow(dialog_), 96);
        OffsetRect(&button, 0, extra);
        size.bottom += extra;
    }
    ShowWindow(control, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    SetWindowPos(GetDlgItem(dialog_, IDOK), nullptr, button.left, button.top,
        button.right - button.left, button.bottom - button.top, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT frame{}, client{};
    GetWindowRect(dialog_, &frame);
    GetClientRect(dialog_, &client);
    SetWindowPos(dialog_, nullptr, 0, 0,
        size.right + frame.right - frame.left - client.right,
        size.bottom + frame.bottom - frame.top - client.bottom,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::SaveSettings() {
    const auto updated = shortcutDraft_;
    if (!IsValidShortcut(updated)) {
        SetStatus(L"Shortcut is unavailable.");
        return;
    }

    const bool newHotkey = !hotkeyId_ || updated != settings_;
    const int candidateId = hotkeyId_ == 1 ? 2 : 1;
    if (newHotkey && !RegisterHotKey(window_, candidateId,
            updated.modifiers | MOD_NOREPEAT, updated.key)) {
        SetStatus(hotkeyError);
        return;
    }

    if (!WriteSettings(updated)) {
        if (newHotkey) {
            UnregisterHotKey(window_, candidateId);
        }
        SetStatus(L"Could not save settings.");
        return;
    }

    if (newHotkey) {
        if (hotkeyId_) {
            UnregisterHotKey(window_, hotkeyId_);
        }
        hotkeyId_ = candidateId;
    }
    settings_ = updated;
    DestroyWindow(dialog_);
}

void App::BeginCapture() {
    try {
        POINT cursor{};
        GetCursorPos(&cursor);
        if (!overlay_.Open(instance_, window_, copyMessage, cursor, scanRegionMessage, cancelRegionMessage)) {
            Notify(L"Could not capture the screen.");
            return;
        }
        ++captureId_;
    } catch (...) {
        overlay_.Close();
        Notify(L"Could not start text capture.");
    }
}

void App::BeginRegionScan(HWND source, size_t gesture) {
    if (!overlay_.IsCurrent(gesture) ||
        GetWindowLongPtrW(source, GWLP_HWNDPARENT) != reinterpret_cast<LONG_PTR>(window_)) return;
    try {
        auto image = overlay_.SnapshotForOcr();
        if (image.pixels.empty()) return;
        ocrGesture_ = gesture;
        worker_->Submit(++captureId_, std::move(image));
    } catch (...) {
        overlay_.Close(true);
        Notify(L"Could not scan the selected region.");
    }
}

void App::CopySelection(HWND source, size_t gesture) {
    const auto text = overlay_.SelectedText(source, gesture);
    if (!text.empty()) {
        CopyToClipboard(text);
    }
}

void App::CopyToClipboard(std::wstring_view text) {
    if (CopyText(window_, text)) {
        overlay_.Close(true);
    } else {
        Notify(L"Could not copy text to the clipboard.");
    }
}

void App::FinishOcr() {
    auto result = worker_ ? worker_->TakeResult() : std::nullopt;
    if (!result || result->captureId != captureId_ || !overlay_.IsOpen() ||
        !overlay_.IsCurrent(ocrGesture_)) {
        return;
    }
    if (result->error) {
        overlay_.Close();
        Notify(result->error);
        return;
    }
    try {
        overlay_.SetText(std::move(result->layout));
    } catch (...) {
        overlay_.Close();
        Notify(L"Could not select the recognized text.");
    }
}
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, identity::mutexName);
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto running = FindWindowW(windowClass, nullptr)) {
            PostMessageW(running, showSettingsMessage, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    int result = 1;
    try {
        result = App(instance).Run();
    } catch (...) {
        MessageBoxW(nullptr, L"Could not start SelectAnyText.", appName, MB_OK | MB_ICONERROR);
    }
    CloseHandle(mutex);
    return result;
}
