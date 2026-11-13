#include "settings_theme.h"
#include "resource.h"

#include <dwmapi.h>

#include <algorithm>
#include <iterator>

namespace {
bool DarkMode() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && !light;
}

COLORREF Mix(COLORREF first, COLORREF second, int amount) {
    const auto channel = [amount](int a, int b) { return (a * (100 - amount) + b * amount) / 100; };
    return RGB(channel(GetRValue(first), GetRValue(second)),
        channel(GetGValue(first), GetGValue(second)), channel(GetBValue(first), GetBValue(second)));
}
}

SettingsTheme::~SettingsTheme() {
    if (brush_) DeleteObject(brush_);
    if (shortcutFont_) DeleteObject(shortcutFont_);
}

void SettingsTheme::Update(HWND dialog) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const BOOL dark = !highContrast && DarkMode();
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(dialog, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    if (highContrast) {
        background_ = surface_ = GetSysColor(COLOR_WINDOW);
        foreground_ = border_ = status_ = GetSysColor(COLOR_WINDOWTEXT);
        accent_ = GetSysColor(COLOR_HIGHLIGHT);
        accentText_ = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else {
        background_ = dark ? RGB(32, 32, 32) : RGB(249, 249, 249);
        foreground_ = dark ? RGB(245, 245, 245) : RGB(27, 27, 27);
        surface_ = dark ? RGB(44, 44, 44) : RGB(255, 255, 255);
        border_ = dark ? RGB(71, 71, 71) : RGB(219, 219, 219);
        status_ = dark ? RGB(255, 166, 166) : RGB(161, 36, 36);
        DWORD color = 0;
        BOOL opaque = FALSE;
        accent_ = SUCCEEDED(DwmGetColorizationColor(&color, &opaque))
            ? RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF)
            : GetSysColor(COLOR_HIGHLIGHT);
        const int brightness = 299 * GetRValue(accent_) + 587 * GetGValue(accent_) + 114 * GetBValue(accent_);
        accentText_ = brightness >= 145'000 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    }
    // Keep the native caption flush with the client area, independent of the
    // Windows accent color. Let Windows own the colors in high contrast mode.
    const COLORREF caption = highContrast ? DWMWA_COLOR_DEFAULT : background_;
    const COLORREF captionText = highContrast ? DWMWA_COLOR_DEFAULT : foreground_;
    DwmSetWindowAttribute(dialog, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(dialog, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(background_);
    if (shortcutFont_) DeleteObject(shortcutFont_);
    shortcutFont_ = nullptr;
    LOGFONTW font{};
    const auto base = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    if (base && GetObjectW(base, sizeof(font), &font)) {
        font.lfHeight = MulDiv(font.lfHeight, 12, 10);
        font.lfWeight = FW_SEMIBOLD;
        shortcutFont_ = CreateFontIndirectW(&font);
    }
    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

INT_PTR SettingsTheme::Color(HDC dc, HWND control) const {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, control && GetDlgCtrlID(control) == IDC_STATUS ? status_ : foreground_);
    return reinterpret_cast<INT_PTR>(brush_ ? brush_ : GetSysColorBrush(COLOR_WINDOW));
}

bool SettingsTheme::Draw(const DRAWITEMSTRUCT& item) const {
    const bool button = item.CtlID == IDOK;
    if (!button && item.CtlID != IDC_SHORTCUT) {
        return false;
    }
    const auto dc = item.hDC;
    const auto saved = SaveDC(dc);
    const auto rect = item.rcItem;
    FillRect(dc, &rect, brush_ ? brush_ : GetSysColorBrush(COLOR_WINDOW));
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto fill = !button ? surface_ : disabled ? Mix(accent_, background_, 75)
        : pressed ? Mix(accent_, background_, 20) : accent_;
    const auto edge = button ? fill : border_;
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, edge);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
    const auto radius = std::max(6, MulDiv(10, GetDpiForWindow(item.hwndItem), 96));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    wchar_t title[128]{};
    GetWindowTextW(item.hwndItem, title, static_cast<int>(std::size(title)));
    const auto font = !button && shortcutFont_ ? shortcutFont_
        : reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font) SelectObject(dc, font);
    auto textRect = rect;
    InflateRect(&textRect, -MulDiv(10, GetDpiForWindow(item.hwndItem), 96), 0);
    HFONT fitted = nullptr;
    SIZE textSize{};
    LOGFONTW adjusted{};
    if (!button && font && GetTextExtentPoint32W(dc, title, lstrlenW(title), &textSize) &&
        textSize.cx > textRect.right - textRect.left && GetObjectW(font, sizeof(adjusted), &adjusted)) {
        adjusted.lfHeight = MulDiv(adjusted.lfHeight, textRect.right - textRect.left, textSize.cx);
        fitted = CreateFontIndirectW(&adjusted);
        if (fitted) SelectObject(dc, fitted);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, button ? (disabled ? foreground_ : accentText_) : foreground_);
    DrawTextW(dc, title, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (button && (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&textRect, -4, -4);
        DrawFocusRect(dc, &textRect);
    }
    RestoreDC(dc, saved);
    if (fitted) DeleteObject(fitted);
    DeleteObject(pen);
    DeleteObject(brush);
    return true;
}
