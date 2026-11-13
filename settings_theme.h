#pragma once

#include <windows.h>

class SettingsTheme {
public:
    ~SettingsTheme();
    void Update(HWND dialog);
    INT_PTR Color(HDC dc, HWND control) const;
    bool Draw(const DRAWITEMSTRUCT& item) const;

private:
    HBRUSH brush_ = nullptr;
    HFONT shortcutFont_ = nullptr;
    COLORREF background_ = RGB(249, 249, 249);
    COLORREF foreground_ = RGB(27, 27, 27);
    COLORREF surface_ = RGB(255, 255, 255);
    COLORREF border_ = RGB(220, 220, 220);
    COLORREF accent_ = RGB(0, 95, 184);
    COLORREF accentText_ = RGB(255, 255, 255);
    COLORREF status_ = RGB(168, 0, 0);
};
